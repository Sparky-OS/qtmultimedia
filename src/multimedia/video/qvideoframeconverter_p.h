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

/**
 *  @brief Maps the video frame and returns an image having a shared ownership for the video frame
 * and referencing to its mapped data.
 */
Q_MULTIMEDIA_EXPORT QImage videoFramePlaneAsImage(QVideoFrame &frame, int plane,
                                                  QImage::Format targetFromat, QSize targetSize);

QT_END_NAMESPACE

#endif

