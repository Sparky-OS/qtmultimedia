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

void QQuickVideoStereoPresenter::setRect(const QRect &rect)
{
    if (m_valid)
        m_layer->setRect(rect);
}

void QQuickVideoStereoPresenter::hide()
{
    if (m_valid)
        m_layer->hide();
}

void QQuickVideoStereoPresenter::submit(Job job)
{
    QMutexLocker lock(&m_mutex);
    // only the newest frame is kept, when the video is faster than the conversion
    m_pending = std::move(job);
    m_hasPending = true;
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
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_hasPending && !m_stopping)
                m_condition.wait(&m_mutex);
            if (m_stopping)
                break;
            job = std::move(m_pending);
            m_pending = {};
            m_hasPending = false;
        }
        process(job);
    }
    m_layer.reset();
}

void QQuickVideoStereoPresenter::process(Job &job)
{
    const QVideoFrameFormat format = job.frame.surfaceFormat();
    const QImage image = qImageFromVideoFrame(
            job.frame, job.wide ? QVideoFrameImageDepth::Float32 : QVideoFrameImageDepth::Float16);
    if (image.isNull())
        return;
    job.frame = {};

    // which of the packing's views is for the left eye
    const bool rightFirst = job.packing.rightEyeFirst != job.swapEyes;
    QImage left, right;
    if (job.packing.mode == Mode::FrameSequence) {
        // each frame is one view (frameView is the eye of the first), the other frame is the other eye's
        const QImage other = qImageFromVideoFrame(
                job.otherFrame, job.wide ? QVideoFrameImageDepth::Float32 : QVideoFrameImageDepth::Float16);
        QImage mine = extractView(image, job.packing, 0, job.scaling);
        QImage theirs = extractView(other, job.packing, 0, job.scaling);
        if (mine.isNull() || theirs.isNull() || mine.size() != theirs.size())
            return;
        left = job.frameView == 0 ? mine : theirs;
        right = job.frameView == 0 ? theirs : mine;
    } else {
        left = extractView(image, job.packing, rightFirst ? 1 : 0, job.scaling);
        right = extractView(image, job.packing, rightFirst ? 0 : 1, job.scaling);
    }
    if (left.isNull() || right.isNull())
        return;

    const BufferFormat bufferFormat = m_layer->chooseFormat(job.format);
    m_layer->setColor({ format.colorSpace(), format.colorTransfer(), format.colorRange(), format.maxLuminance() });
    m_layer->present(left, right, bufferFormat);

    if (qLcStereoPresenter().isDebugEnabled()) {
        // the number of different levels of red in each view as the buffer holds them
        auto halfBits = [](float v) {
            const qfloat16 h(v);
            quint16 bits;
            memcpy(&bits, &h, sizeof bits);
            return int(bits);
        };
        auto levels = [bufferFormat, halfBits](const QImage &view) {
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
        qCDebug(qLcStereoPresenter) << "frame" << format.frameSize() << "image" << image.size() << "packing"
                                    << int(job.packing.mode) << int(job.packing.size) << "right first" << job.packing.rightEyeFirst << "swap" << job.swapEyes << "frame eye" << job.frameView << "stereo view" << int(format.stereoView()) << "view" << left.size() << "format"
                                    << (bufferFormat == BufferFormat::Xrgb8888 ? "XRGB8888"
                                        : bufferFormat == BufferFormat::Xrgb2101010 ? "XRGB2101010"
                                        : bufferFormat == BufferFormat::Xbgr16161616 ? "XBGR16161616" : "XBGR16161616F")
                                    << "pixel format" << format.pixelFormat() << "levels of red, left" << levels(left)
                                    << "right" << levels(right);
    }
}

QT_END_NAMESPACE
