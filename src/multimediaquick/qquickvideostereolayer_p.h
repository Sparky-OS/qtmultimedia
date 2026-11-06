// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QQUICKVIDEOSTEREOLAYER_P_H
#define QQUICKVIDEOSTEREOLAYER_P_H

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
#include <QtGui/qimage.h>
#include <QtMultimedia/qvideoframeformat.h>
#include <QtMultimediaQuick/private/qquickvideostereo_p.h>
#include <QtMultimediaQuick/private/qquickvideodepth_p.h>

#include <memory>

QT_BEGIN_NAMESPACE

class QWindow;

// The 3D area of a window as a surface of its own, under the window's: a Wayland subsurface that
// holds both views of a stereoscopic 3D video side by side, left first, and is declared to the
// compositor as full side by side stereo content (kde_stereo_content_manager_v1) so that the
// compositor shows each view to its eye, while the window's own surface keeps the controls over
// it, the same in both eyes (the separation of the 2D part from the 3D area).
//
// The window has to have an alpha buffer from its creation, and its scene graph has to leave the
// area transparent, for the layer to be seen under the window's surface. The functions may be
// called from any thread, but not at the same time.
class QQuickVideoStereoLayer
{
public:
    // Whether this compositor has what the layer needs: wl_subcompositor, wp_viewporter, wl_shm and
    // the stereo content declaration
    static bool isSupported();
    static std::unique_ptr<QQuickVideoStereoLayer> create(QWindow *window);

    ~QQuickVideoStereoLayer();

    // The buffer format of the layer: the one asked for if wl_shm offers it, else the deepest
    // wl_shm offers that is not deeper, else 8 bits
    QQuickVideoStereo::BufferFormat chooseFormat(QQuickVideoStereo::BufferFormat wanted) const;

    // Shows the two native views at one view's rectangle in logical window pixels.
    // The buffer and its destination are committed together. False means
    // backpressure: retry the latest matching pair when buffers are released.
    bool present(const QImage &left, const QImage &right, QQuickVideoStereo::BufferFormat format,
                 const QRect &rect);
    // Shows the views of a frame of textures (the drawn views of a depth video) without a read back: the layer makes an EGL context that shares the
    // drawing context's textures, a window surface for its wl_surface (the driver exchanges the buffers with the compositor as DMA-BUF where both
    // take them), and draws the two eyes into it with one pass at the texture's depth. crop is the part of one eye shown, in the eye's pixels.
    // False with the reason in why: the caller then shows the views through present(), which leaves the window surface. *used is the format of
    // the surface: the one asked for where the driver has a surface of it, else the next one down (said in the log).
    bool presentGpu(const QQuickVideoDepth::LayerSource &source, bool swapEyes, const QRect &crop,
                    QQuickVideoStereo::BufferFormat format, const QRect &rect,
                    QQuickVideoStereo::BufferFormat *used, QString *why);
    // Whether the last view was shown through a window surface of the GPU
    bool gpuActive() const;
    // Whether the compositor offers zwp_linux_dmabuf_v1
    bool compositorHasDmabuf() const;
    // Takes the layer off the screen until the next present()
    void hide();

    // The color description of the video, for the compositor where it takes one
    struct Color
    {
        QVideoFrameFormat::ColorSpace space = QVideoFrameFormat::ColorSpace_Undefined;
        QVideoFrameFormat::ColorTransfer transfer = QVideoFrameFormat::ColorTransfer_Unknown;
        QVideoFrameFormat::ColorRange range = QVideoFrameFormat::ColorRange_Unknown;
        float maxLuminance = -1; // of the mastering display, cd/m2, if the stream says
    };
    void setColor(const Color &color);
    // Whether the compositor takes color descriptions
    bool hasColorManagement() const;
    // Whether a video with this color description can be shown in a layer: SDR video in the
    // primaries of BT.709 needs no description, the other needs the compositor to take it;
    // otherwise the layer would show its code values as sRGB
    static bool canDescribe(const Color &color);

private:
    QQuickVideoStereoLayer();
    struct Private;
    std::unique_ptr<Private> d;
};

QT_END_NAMESPACE

#endif
