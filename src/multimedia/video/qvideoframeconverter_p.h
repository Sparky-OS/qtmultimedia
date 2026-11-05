// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QVIDEOFRAMECONVERTER_H
#define QVIDEOFRAMECONVERTER_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <qvideoframe.h>
#include <QtMultimedia/private/qmultimediautils_p.h>
#include <QtGui/qimage.h>
#include <QtGui/qtransform.h>
#include <QtMultimedia/private/qvideotexturehelper_p.h>
#include <cstring>

QT_BEGIN_NAMESPACE

struct VideoTransformation;

// How deep the image of qImageFromVideoFrame() is. The 8 bits of the default are what QImage
// has had since the beginning; the floating point images hold the samples of a high bit depth
// video, which the color matrix and the transfer function apply to, without rounding them to
// 8 bits. In them PQ and HLG video keeps its code values (as the 8 bit image does for the
// other transfer functions of the video, whose range is cut to the SDR of the output only by
// the scene graph's own nodes)
enum class QVideoFrameImageDepth
{
    Bits8,
    Float16,
    Float32
};

Q_MULTIMEDIA_EXPORT QImage qImageFromVideoFrame(const QVideoFrame &frame,
                                                const VideoTransformation &transformation,
                                                QVideoFrameImageDepth depth, bool forceCpu = false);

// Float16 and Float32 give an RGBA16FPx4 or RGBA32FPx4 premultiplied image; where no GPU
// rendering is available, or it cannot render to floating point, the 8 bit image
Q_MULTIMEDIA_EXPORT QImage qImageFromVideoFrame(const QVideoFrame &frame,
                                                QVideoFrameImageDepth depth);

Q_MULTIMEDIA_EXPORT QImage qImageFromVideoFrame(const QVideoFrame &frame,
                                                const VideoTransformation &transformation,
                                                bool forceCpu = false);

Q_MULTIMEDIA_EXPORT QImage qImageFromVideoFrame(const QVideoFrame &frame, bool forceCpu = false);

// Give each decoded view its own native planes before chroma reconstruction.
// Converting the packed YUV image first would interpolate across the eye boundary.
inline QVideoFrame qVideoFrameView(const QVideoFrame &frame, int eye, bool preserveTransform = true)
{
    auto format = frame.surfaceFormat();
    const QSize size(frame.width() / 2, frame.height());
    format.setFrameSize(size);
    format.setViewport(QRect(QPoint(), size));
    format.setStereoMode(QVideoFrameFormat::StereoMode::None);
    format.setStereoFlags({});
    if (!preserveTransform) {
        format.setRotation(QtVideo::Rotation::None);
        format.setMirrored(false);
    }
    QVideoFrame source(frame), view(format);
    if (!source.map(QVideoFrame::ReadOnly))
        return {};
    if (!view.map(QVideoFrame::WriteOnly)) {
        source.unmap();
        return {};
    }
    const auto *description = QVideoTextureHelper::textureDescription(format.pixelFormat());
    bool valid = description && description->nplanes == source.planeCount();
    for (int plane = 0; valid && plane < source.planeCount(); ++plane) {
        int bytes = 0;
        switch (description->textureFormat[plane]) {
        case QVideoTextureHelper::TextureDescription::Red_8: bytes = 1; break;
        case QVideoTextureHelper::TextureDescription::RG_8:
        case QVideoTextureHelper::TextureDescription::Red_16: bytes = 2; break;
        case QVideoTextureHelper::TextureDescription::RG_16:
        case QVideoTextureHelper::TextureDescription::RGBA_8:
        case QVideoTextureHelper::TextureDescription::BGRA_8: bytes = 4; break;
        default: valid = false; continue;
        }
        const int width = description->widthForPlane(size.width(), plane);
        const int height = description->heightForPlane(size.height(), plane);
        const int row = width * bytes;
        valid = description->widthForPlane(frame.width(), plane) == 2 * width
                && row <= view.bytesPerLine(plane) && 2 * row <= source.bytesPerLine(plane);
        for (int y = 0; valid && y < height; ++y)
            std::memcpy(view.bits(plane) + y * view.bytesPerLine(plane),
                        source.bits(plane) + y * source.bytesPerLine(plane) + (eye ? row : 0), row);
    }
    source.unmap();
    view.unmap();
    if (!valid)
        return {};
    if (preserveTransform) {
        view.setRotation(frame.rotation());
        view.setMirrored(frame.mirrored());
    }
    view.setStartTime(frame.startTime());
    view.setEndTime(frame.endTime());
    return view;
}

inline QImage qImageFromVideoFrameView(const QVideoFrame &frame, int eye,
                                       QVideoFrameImageDepth depth)
{
    QImage view = qImageFromVideoFrame(qVideoFrameView(frame, eye, false), VideoTransformation{}, depth);
    const auto transform = qNormalizedFrameTransformation(frame);
    if (transform.rotation != QtVideo::Rotation::None)
        view = view.transformed(QTransform().rotate(qToUnderlying(transform.rotation)));
    if (transform.mirroredHorizontallyAfterRotation)
        view = view.flipped(Qt::Horizontal);
    return view;
}

/**
 *  @brief Maps the video frame and returns an image having a shared ownership for the video frame
 * and referencing to its mapped data.
 */
Q_MULTIMEDIA_EXPORT QImage videoFramePlaneAsImage(QVideoFrame &frame, int plane,
                                                  QImage::Format targetFromat, QSize targetSize);

QT_END_NAMESPACE

#endif

