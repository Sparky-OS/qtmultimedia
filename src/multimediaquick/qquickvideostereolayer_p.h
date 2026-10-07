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

    // The rectangle of one view in the window's surface, in logical pixels
    void setRect(const QRect &rect);

    // Shows the two views, each as large as the image; both images have the same size
    void present(const QImage &left, const QImage &right, QQuickVideoStereo::BufferFormat format);
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
