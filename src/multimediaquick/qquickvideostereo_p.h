// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QQUICKVIDEOSTEREO_P_H
#define QQUICKVIDEOSTEREO_P_H

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

#include <QtCore/qsize.h>
#include <QtGui/qimage.h>
#include <QtMultimedia/qvideoframeformat.h>
#include <QtMultimediaQuick/private/qtmultimediaquickglobal_p.h>

QT_BEGIN_NAMESPACE

// Taking the two views of a stereoscopic 3D frame apart, in the packings of H.264 Table D-8,
// H.265 Table D.8 and Matroska's StereoMode (RFC 9559, 5.1.4.1.28.3)
namespace QQuickVideoStereo {

enum class Scaling { Bilinear, Bicubic, Lanczos };

// How the frame holds its views. The side by side and top and bottom packings can be "half" (each
// view squeezed to half the frame, the shape of 3D television) or "full" (each view at full
// size, the frame twice as wide or as tall)
struct Packing
{
    QVideoFrameFormat::StereoMode mode = QVideoFrameFormat::StereoMode::None;
    bool rightEyeFirst = false;
    enum class Size { ByShape, Half, Full } size = Size::ByShape;
    // The size the frame is shown at, when it is not the size it is stored in (a frame the
    // backend left unscaled to the shape of its pixels, QVideoFrameFormat::pixelAspectRatio()):
    // the views are then restored from the stored samples to the size of a view of this
    QSize displaySize;
};

// True for the packings whose views can be taken apart (the anaglyphs hold both in the colors)
Q_MULTIMEDIAQUICK_EXPORT bool hasTwoViews(QVideoFrameFormat::StereoMode mode);
// Whether the side by side or top and bottom frame of this size holds full size views: the
// frame is at least 2.5 times as wide as high (32:9, two 16:9 pictures next to each other), or
// at most 1.2 times as wide as high (two 16:9 pictures on top of each other is 8:9 or 16:18)
Q_MULTIMEDIAQUICK_EXPORT bool isFullSize(const Packing &packing, QSize frameSize);
// The size of one view at full resolution: the squeezed side of a half view is restored, and
// the views of the interleaved packings have the size of the frame
Q_MULTIMEDIAQUICK_EXPORT QSize viewSize(const Packing &packing, QSize frameSize);
// The size of the view as it is in the frame, where it is a rectangle of the frame (side by
// side and top and bottom), and that rectangle, in pixels of the frame; first = 0 or 1
Q_MULTIMEDIAQUICK_EXPORT bool viewRect(const Packing &packing, QSize frameSize, int first, QRect *rect);

// Takes the view that is first (0) or second (1) in the packing out of the image, in the size
// of viewSize(). The image is any RGBA float image or an 8 bit one; the result is RGBA32FPx4
Q_MULTIMEDIAQUICK_EXPORT QImage extractView(const QImage &frame, const Packing &packing, int index, Scaling scaling);

// Writes the pair left|right into memory as the given Wayland shm format, the layer's buffer:
// 8 bits (XRGB8888), 10 bits (XRGB2101010), 16 bits (XBGR16161616) or half float (XBGR16161616F)
enum class BufferFormat { Xrgb8888, Xrgb2101010, Xbgr16161616, Xbgr16161616f };
Q_MULTIMEDIAQUICK_EXPORT int bytesPerPixel(BufferFormat format);
Q_MULTIMEDIAQUICK_EXPORT void writePair(const QImage &left, const QImage &right, BufferFormat format, uchar *dst, int stride);

} // namespace QQuickVideoStereo

QT_END_NAMESPACE

#endif
