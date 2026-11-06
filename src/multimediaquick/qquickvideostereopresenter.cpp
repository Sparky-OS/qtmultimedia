// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideostereopresenter_p.h"

#include <QtMultimedia/private/qvideoframeconverter_p.h>
#include <QtMultimediaQuick/private/qquickvideodepth_p.h>
#include <QtCore/qloggingcategory.h>
#include <QtCore/qfloat16.h>

#include <algorithm>
#include <vector>

QT_BEGIN_NAMESPACE

Q_STATIC_LOGGING_CATEGORY(qLcStereoPresenter, "qt.multimedia.video.stereopresenter")

using namespace QQuickVideoStereo;
using Mode = QVideoFrameFormat::StereoMode;

QQuickVideoStereoPresenter::QQuickVideoStereoPresenter(QWindow *window)
    : m_window(window)
{
    // the diagnostic: the views are always converted and copied into wl_shm buffers (the control of the layer's GPU path)
    m_forceShm = qEnvironmentVariableIntValue("QT_MM_STEREO_LAYER_SHM") != 0;
    start();
    QMutexLocker lock(&m_mutex);
    while (!m_created)
        m_condition.wait(&m_mutex);
}

QQuickVideoStereoPresenter::~QQuickVideoStereoPresenter()
{
    {
        QMutexLocker lock(&m_mutex);
        m_stopping = true;
        m_condition.wakeAll();
    }
    wait();
}

void QQuickVideoStereoPresenter::setGeometry(Geometry geometry)
{
    QMutexLocker lock(&m_mutex);
    m_state.setGeometry(geometry);
    m_condition.wakeAll();
}

void QQuickVideoStereoPresenter::hide()
{
    QMutexLocker lock(&m_mutex);
    m_state.hide();
    m_condition.wakeAll();
}

void QQuickVideoStereoPresenter::clear()
{
    QMutexLocker lock(&m_mutex);
    m_state.clear();
    m_condition.wakeAll();
}

QQuickVideoStereoPresenter::LayerRoute QQuickVideoStereoPresenter::layerRoute() const
{
    QMutexLocker lock(&m_routeMutex);
    return m_route;
}

void QQuickVideoStereoPresenter::submit(Job job)
{
    QMutexLocker lock(&m_mutex);
    m_state.submit(std::move(job));
    m_condition.wakeAll();
}

void QQuickVideoStereoPresenter::run()
{
    m_layer = QQuickVideoStereoLayer::create(m_window);
    {
        QMutexLocker lock(&m_mutex);
        m_valid = m_layer != nullptr;
        m_created = true;
        m_condition.wakeAll();
    }
    while (m_valid) {
        State::Work work;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_state.dirty && !m_stopping)
                m_condition.wait(&m_mutex);
            // wl_buffer releases are dispatched by present(). Retry a paused
            // update after backpressure without spinning or needing a new frame.
            if (m_state.retry && !m_stopping)
                m_condition.wait(&m_mutex, 16);
            if (m_stopping)
                break;
            work = m_state.take();
        }
        process(std::move(work));
    }
    m_layer.reset();
}

void QQuickVideoStereoPresenter::process(State::Work work)
{
    if (!work.visible) {
        QMutexLocker lock(&m_mutex);
        if (m_state.isCurrent(work.revision))
            m_layer->hide();
        return;
    }
    Job &job = work.job;
    const QVideoFrameFormat format = job.frame.surfaceFormat();
    const bool rightFirst = job.packing.rightEyeFirst != job.swapEyes;
    const auto depth = job.wide ? QVideoFrameImageDepth::Float32 : QVideoFrameImageDepth::Float16;
    QString gpuWhy;
    // The drawn views of a depth video are textures of the drawing context: the layer draws them into a window surface of the GPU, with no read back
    // and no conversion or copy of ours (the driver and the compositor exchange the buffers, as DMA-BUF where both take them)
    if (m_forceShm) {
        gpuWhy = QStringLiteral("QT_MM_STEREO_LAYER_SHM asks for wl_shm buffers");
    } else if (format.stereoFlags().testFlag(QVideoFrameFormat::StereoFlag::Multiview) || job.packing.mode != Mode::SideBySide
               || job.packing.size != Packing::Size::Full) {
        gpuWhy = QStringLiteral("the frame is not the full side by side the drawing passes make");
    } else {
        QQuickVideoDepth::LayerSource source;
        if (!QQuickVideoDepth::layerSourceOf(job.frame, &source)) {
            gpuWhy = QStringLiteral("the frame is not made of textures of a drawing context the layer can share");
        } else {
            const QSize eye(source.width / 2, source.height);
            QRect crop(QPoint(0, 0), eye);
            if (work.geometry.crop)
                crop = coverCrop(eye, job.displaySize, work.geometry.destination.size());
            if (crop.isEmpty()) {
                gpuWhy = QStringLiteral("the crop of the views is empty");
            } else {
                // the revision check and the commit stay together, as for the wl_shm buffers
                QMutexLocker lock(&m_mutex);
                if (!m_state.isCurrent(work.revision))
                    return;
                m_layer->setColor({ format.colorSpace(), format.colorTransfer(), format.colorRange(), format.maxLuminance() });
                BufferFormat used = job.format;
                if (m_layer->presentGpu(source, rightFirst, crop, job.format, work.geometry.destination, &used, &gpuWhy)) {
                    QMutexLocker routeLock(&m_routeMutex);
                    ++m_route.gpuFrames;
                    m_route.gpuActive = true;
                    m_route.dmabuf = m_layer->compositorHasDmabuf();
                    job.frame = {};
                    return;
                }
            }
        }
    }
    {
        QMutexLocker routeLock(&m_routeMutex);
        ++m_route.copiedFrames;
        m_route.gpuActive = false;
        if (m_route.why != gpuWhy)
            qCInfo(qLcStereoPresenter).noquote() << "the views are converted and copied into wl_shm buffers by the presenter:" << gpuWhy;
        m_route.why = gpuWhy;
        if (m_layer)
            m_route.dmabuf = m_layer->compositorHasDmabuf();
    }
    QImage left, right;
    if (format.stereoFlags().testFlag(QVideoFrameFormat::StereoFlag::Multiview)) {
        left = qImageFromVideoFrameView(job.frame, rightFirst ? 1 : 0, depth);
        right = qImageFromVideoFrameView(job.frame, rightFirst ? 0 : 1, depth);
    } else {
        const QImage image = qImageFromVideoFrame(job.frame, depth);
        if (image.isNull())
            return;
        if (job.packing.mode == Mode::FrameSequence) {
            const QImage other = qImageFromVideoFrame(job.otherFrame, depth);
            QImage mine = storedView(image, job.packing, 0);
            QImage theirs = storedView(other, job.packing, 0);
            if (mine.isNull() || theirs.isNull() || mine.size() != theirs.size())
                return;
            left = job.frameView == 0 ? mine : theirs;
            right = job.frameView == 0 ? theirs : mine;
        } else {
            left = storedView(image, job.packing, rightFirst ? 1 : 0);
            right = storedView(image, job.packing, rightFirst ? 0 : 1);
        }
    }
    job.frame = {};
    if (left.isNull() || right.isNull())
        return;

    if (work.geometry.crop) {
        const QRect crop = coverCrop(left.size(), job.displaySize, work.geometry.destination.size());
        if (crop.isEmpty() || left.size() != right.size())
            return;
        if (crop != left.rect()) {
            left = left.copy(crop);
            right = right.copy(crop);
        }
    }

    // Keep the revision check and the Wayland commit together. Conversion above
    // may finish after a resize, hide or 3D-to-2D transition.
    QMutexLocker lock(&m_mutex);
    if (!m_state.isCurrent(work.revision))
        return;
    const BufferFormat bufferFormat = m_layer->chooseFormat(job.format);
    m_layer->setColor({ format.colorSpace(), format.colorTransfer(), format.colorRange(), format.maxLuminance() });
    if (!m_layer->present(left, right, bufferFormat, work.geometry.destination)) {
        m_state.retryPresentation(work.revision);
        return;
    }

    if (qLcStereoPresenter().isDebugEnabled()) {
        // the number of different levels of red in each view as the buffer holds them
        auto halfBits = [](float v) {
            const qfloat16 h(v);
            quint16 bits;
            memcpy(&bits, &h, sizeof bits);
            return int(bits);
        };
        auto levels = [bufferFormat, halfBits](const QImage &image) {
            const QImage view = image.format() == QImage::Format_RGBA32FPx4_Premultiplied
                    ? image : image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
            std::vector<bool> seen(65536, false);
            for (int y = 0; y < view.height(); ++y) {
                const float *line = reinterpret_cast<const float *>(view.constScanLine(y));
                for (int x = 0; x < view.width(); ++x) {
                    const float v = std::clamp(line[x * 4], 0.f, 1.f);
                    const int q = bufferFormat == BufferFormat::Xrgb8888 ? int(v * 255.f + 0.5f)
                            : bufferFormat == BufferFormat::Xrgb2101010 ? int(v * 1023.f + 0.5f)
                            : bufferFormat == BufferFormat::Xbgr16161616 ? int(v * 65535.f + 0.5f)
                            : halfBits(v);
                    seen[q] = true;
                }
            }
            return std::count(seen.begin(), seen.end(), true);
        };
        qCDebug(qLcStereoPresenter) << "frame" << format.frameSize() << "packing"
                                    << int(job.packing.mode) << int(job.packing.size) << "right first" << job.packing.rightEyeFirst << "swap" << job.swapEyes << "frame eye" << job.frameView << "stereo view" << int(format.stereoView()) << "view" << left.size() << "format"
                                    << (bufferFormat == BufferFormat::Xrgb8888 ? "XRGB8888"
                                        : bufferFormat == BufferFormat::Xrgb2101010 ? "XRGB2101010"
                                        : bufferFormat == BufferFormat::Xbgr16161616 ? "XBGR16161616" : "XBGR16161616F")
                                    << "pixel format" << format.pixelFormat() << "levels of red, left" << levels(left)
                                    << "right" << levels(right);
    }
}

QT_END_NAMESPACE
