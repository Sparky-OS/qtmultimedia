// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QFFMPEGDEPTH_P_H
#define QFFMPEGDEPTH_P_H

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

#include <QtFFmpegMediaPluginImpl/private/qffmpeg_p.h>
#include <QtMultimedia/qvideoframeformat.h>
#include <QtCore/qbytearray.h>

#include <optional>

QT_BEGIN_NAMESPACE

// Depth as a 3D source: a picture with its depth map (2D plus depth), from which the consumer
// draws the two views (QVideoFrameFormat's depth layouts). The layouts and the meaning of the
// samples are the marks of Sparky Stereo OS (stereo-layer, docs/depth-marks.md); FFmpeg says them
// in its depth side data, the AuxId of a depth layer or stream, and a Matroska track's tags.
namespace QFFmpeg {

// Whether the edition's FFmpeg is there, which exports the depth (libavutil/multiview.h)
bool depthSupported();

// Whether depth maps are decoded with the pictures: with the edition's FFmpeg, unless
// QT_FFMPEG_VIDEO_DEPTH=0 says not to (then a video with its depth map shows its picture alone)
bool depthWanted();

// The AuxId of an auxiliary video stream (H.265 Table F.2: 2 a depth map), as the demuxer says
// it (an ISO/IEC 23002-3 stream, MPEG-2 transport stream type 0x1E, or an MP4 auxiliary depth
// track): 2 for a depth or parallax map, 0 for a stream that is not auxiliary
int streamAuxiliaryId(const AVStream *stream);

// Whether the auxiliary stream is an ISO/IEC 23002-3 parallax map, not a depth map proper
bool streamIsParallaxMap(const AVStream *stream);

// The AuxId of a frame of a depth auxiliary layer (FFmpeg's depth_output), 0 for any other
int frameAuxiliaryId(const AVFrame *frame);

// Whether the H.265 decoder outputs the pictures of the stream's depth auxiliary layer: the stream
// has one (depth_available, known once the decoder has read its parameter sets) and they are
// asked for (depth_output)
bool decoderOutputsDepthLayer(AVCodecContext *context);

struct DepthLayout
{
    QVideoFrameFormat::StereoMode mode = QVideoFrameFormat::StereoMode::None;
    QVideoFrameFormat::StereoFlags flags;
};

// The layout a Stereo3DLayout word names, when it is a depth layout (docs/depth-marks.md; an
// unknown word means one picture)
std::optional<DepthLayout> depthLayoutFromWord(QByteArrayView word);

// The depth marks of a stream, from the simple tags of its track (STEREO3D_LAYOUT and
// STEREO3D_DEPTH, until the layouts are registered as Matroska StereoMode values)
struct StreamDepth
{
    std::optional<DepthLayout> layout;
    QString mark;
};
StreamDepth streamDepth(const AVStream *stream);

// What the samples of a depth map mean, as a Stereo3DDepth mark, from the side data of the
// depth frame, else of the picture: the depth representation information of H.264 (H.13.2.3)
// and H.265 (G.14.3.4), the supplemental information of an ISO/IEC 23002-3 stream. Only what the
// stream says is written, so that the user's range, screen and shift apply where it says none.
// Empty when nothing says it. viewWidth is the width of the picture, which DMin and DMax are of.
QString depthMarkFromSideData(const AVFrame *depth, const AVFrame *picture, int viewWidth);

// The picture and its depth map packed side by side in one frame of the picture's pixel format,
// twice as wide: the depth map's first plane, at the picture's size and sample depth, as the luma
// of the right half (neutral chroma). Its metadata says it: STEREO3D_LAYOUT
// depthSideBySidePictureFirst, and STEREO3D_DEPTH from the side data. Null where the picture's
// pixel format has no planes of its own (packed YUV) or a frame cannot be read.
AVFrameUPtr packPictureAndDepth(const AVFrame *picture, const AVFrame *depth);

// The depth marks of a frame's metadata (a frame packed by packPictureAndDepth)
StreamDepth frameDepth(const AVFrame *frame);

} // namespace QFFmpeg

QT_END_NAMESPACE

#endif
