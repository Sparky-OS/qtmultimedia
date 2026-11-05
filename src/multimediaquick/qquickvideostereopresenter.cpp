// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideostereopresenter_p.h"

#include <QtMultimedia/private/qvideoframeconverter_p.h>
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
