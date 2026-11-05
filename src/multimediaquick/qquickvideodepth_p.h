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
class QRhi;

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
    // A frame the decoder keeps on a GPU is read by the passes where it is, no copy to system memory, when the decoder's texture
    // converter can map it into the drawing context; false: it is copied (the log says which). Same views either way.
    bool importFrames = true;
    // The drawn views go to the video sink as textures of the drawing context, shared with the sink's (no read back to system memory), when
    // the sink draws with OpenGL; false: they are read back (the log says which, and why). Same views either way.
    bool viewsAsTexture = true;
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

// What the GPU that draws has free, for the controls to say beside a way that would not fit (the way is the person's to pick, and
// runs even then): bytes the driver says are free (-1 when it does not say) and the size and depth the ways are drawn at
struct Q_MULTIMEDIAQUICK_EXPORT GpuMemory
{
    bool valid = false;
    double freeBytes = -1;
    int width = 0, height = 0, bits = 8;

    bool operator==(const GpuMemory &o) const
    {
        return valid == o.valid && freeBytes == o.freeBytes && width == o.width && height == o.height && bits == o.bits;
    }
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
    // The GPU's free memory as of the last draws on it (not valid when drawing on the processor)
    GpuMemory gpuMemory() const;

    // How the last frame the decoder keeps on a GPU reached the passes: Memory for a frame in system memory (or one this Qt has no
    // converter for: the read back is the decoder's), Imported where it is (no read back), Copied to system memory (why in
    // lastFrameRouteWhy())
    enum class FrameRoute { Memory, Imported, Copied };
    FrameRoute lastFrameRoute() const;
    QString lastFrameRouteWhy() const;
    // The hardware frames this Renderer read back to system memory
    int hardwareReadBacks() const;

    // The video sink the drawn views are shown by: when it draws with OpenGL the drawing context is made to share its textures (the
    // sink's RHI is the scene graph's, set from the thread that shows the frames)
    void setSinkRhi(QRhi *rhi);
    // How the last drawn views reached the sink: Memory (drawn by the C code in a frame of system memory), Texture (the drawing
    // context's textures, no read back) or ReadBack (read back to system memory, why in lastViewsRouteWhy())
    enum class ViewsRoute { Memory, Texture, ReadBack };
    ViewsRoute lastViewsRoute() const;
    QString lastViewsRouteWhy() const;
    // The drawn views read back to system memory: by the drawing, and by whoever mapped a frame of textures (a consumer that is not
    // the sink)
    int viewsReadBacks() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace QQuickVideoDepth

QT_END_NAMESPACE

#endif
