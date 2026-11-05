// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QQUICKVIDEODEPTH_P_H
#define QQUICKVIDEODEPTH_P_H

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

#include <QtCore/qrect.h>
#include <QtCore/qstring.h>
#include <QtMultimedia/qvideoframe.h>
#include <QtMultimedia/qvideoframeformat.h>
#include <QtMultimediaQuick/private/qtmultimediaquickglobal_p.h>

#include <memory>

QT_BEGIN_NAMESPACE

// Depth as a 3D source: the two views of a picture with its depth map (2D plus depth), drawn
// with libstereo-dibr (depth-image-based rendering, Sparky Stereo OS's stereo-layer) into a full
// side by side frame, both views at the picture's size, the left eye first. The layouts and the
// meaning of the samples are the depth marks (stereo-layer, docs/depth-marks.md).
namespace QQuickVideoDepth {

// Every choice of how the views are drawn. The defaults are libstereo-dibr's, the one default of
// the library (stereo_dibr_options_default()): this structure holds no copy of them.
struct Q_MULTIMEDIAQUICK_EXPORT Options
{
    Options();

    bool enabled = true;
    int view = 0;   // 0: the picture is the left eye, 1: the middle
    int warp = 0;   // 0 forward, 1 backward, 2 layered
    int fill = 0;   // 0 background extension, 1 depth-aided, 2 Telea, 3 none, 4 pyramid
    int smooth = 0; // 0 off, 1 asymmetric Gaussian
    double sigmaH = 0, sigmaV = 0; // the Gaussian's, pixels of a view 1920 wide
    int radius = 1;
    double range = 0;  // the largest parallax either side of the screen, pixels of a view 1920 wide
    double zero = 0;   // where the screen is, from the farthest samples (0) to the nearest (1)
    double shift = 0;  // a parallax added to all, pixels of a view 1920 wide
    // The way (warp and fill) is not the person's: a video is drawn the best way that keeps its
    // frame rate on the GPU (or the processor) that draws it, measured once for each GPU and size
    // (stereo_dibr_choose_from_times()). false: warp and fill are the person's, for everything.
    bool automatic = true;
    // Draw on the GPU when it can (OpenGL, libstereo-dibr's passes); false draws with the C code
    // on the processor
    bool gpu = true;
    // -1: what the video says; else a kind of libstereo-dibr (0 nearness .. 9 metric)
    int meaning = -1;
    double zNear = 0, zFar = 0; // the near and far distances, when both are set
    QString mark;      // a whole Stereo3DDepth mark, before the meaning and the video's
};

// Whether the mode is one of the depth layouts
Q_MULTIMEDIAQUICK_EXPORT bool isDepthLayout(QVideoFrameFormat::StereoMode mode);

// The rectangle of the picture in a frame of the depth layout (the picture shown alone)
Q_MULTIMEDIAQUICK_EXPORT QRect pictureRect(QVideoFrameFormat::StereoMode mode, bool depthFirst,
                                           QSize frameSize);

// Whether libstereo-dibr is there to draw the views
Q_MULTIMEDIAQUICK_EXPORT bool canDraw();

// What the automatic way chose for the video that plays, and why: the way, whether it keeps the frame
// rate, the best way it gave up and where it draws
struct Q_MULTIMEDIAQUICK_EXPORT Choice
{
    bool valid = false;
    int warp = 0, fill = 0;
    bool keepsUp = true;
    double fps = 0;
    double ms = 0, budgetMs = 0;
    int skippedWarp = -1, skippedFill = -1;
    double skippedMs = 0;
    bool onGpu = false;
    bool gpuLost = false; // the GPU was reset while drawing: it is not used again for this video
    QString gpu; // the GPU's name, or the processor's

    bool operator==(const Choice &o) const;
};

class Q_MULTIMEDIAQUICK_EXPORT Renderer
{
public:
    enum class Result {
        // the views, full side by side
        Drawn,
        // a malformed Stereo3DDepth mark means one picture (docs/depth-marks.md, rule 1)
        PictureAlone,
        // a frame that cannot be read, or no libstereo-dibr
        Failed
    };

    Renderer();
    ~Renderer();

    // Draws the views of the frame, in the depth layout mode (DepthFirst in depthFirst), into a
    // frame of the same pixel format and color, twice the picture's width, marked
    // StereoMode::SideBySide. fileMark is what the stream says a sample means.
    Result draw(const QVideoFrame &frame, QVideoFrameFormat::StereoMode mode, bool depthFirst,
                const QString &fileMark, const Options &options, QVideoFrame *out);

    // What the last draw() chose (valid only for the automatic way) and where it drew
    Choice choice() const;
    // Whether the last draw() was on the GPU
    bool lastOnGpu() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace QQuickVideoDepth

QT_END_NAMESPACE

#endif
