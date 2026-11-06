// Copyright (C) 2021 The Qt Company Ltd.
// Copyright (C) 2016 Research In Motion
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideooutput_p.h"

#include <QtCore/qloggingcategory.h>
#include <QtCore/qrunnable.h>
#include <QtCore/private/qfactoryloader_p.h>

#include <QtMultimedia/qmediaplayer.h>
#include <QtMultimedia/qmediacapturesession.h>
#include <QtMultimedia/private/qmultimediautils_p.h>
#include <QtMultimedia/private/qvideoframetexturepool_p.h>
#include <QtMultimedia/private/qvideooutputorientationhandler_p.h>

#include <QtMultimedia/private/qvideoframeconverter_p.h>

#include <QtMultimediaQuick/private/qsgvideonode_p.h>
#include <QtMultimediaQuick/private/qquickvideostereo_p.h>
#include <QtMultimediaQuick/private/qquickvideodepth_p.h>
#ifdef QT_MM_STEREO_POLICY
#include <stereo-policy.h>
#endif
#ifdef QT_MM_STEREO_LAYER
#include <QtMultimediaQuick/private/qquickvideostereopresenter_p.h>

#include <QtQuick/qsgflatcolormaterial.h>
#include <QtQuick/qsgsimplerectnode.h>
#endif

#include <QtQuick/qquickwindow.h>
#include <QtQuick/private/qquickwindow_p.h>

QT_BEGIN_NAMESPACE

Q_STATIC_LOGGING_CATEGORY(qLcVideo, "qt.multimedia.video")

namespace {

inline bool qIsDefaultAspect(int o)
{
    return (o % 180) == 0;
}

inline bool qIsDefaultAspect(QtVideo::Rotation rotation)
{
    return qIsDefaultAspect(qToUnderlying(rotation));
}

using StereoMode = QVideoFrameFormat::StereoMode;

// The packing a StereoInput names, and the other way
bool packingFromInput(QQuickVideoOutput::StereoInput input, QQuickVideoStereo::Packing *packing)
{
    using Size = QQuickVideoStereo::Packing::Size;
    switch (input) {
    case QQuickVideoOutput::StereoSideBySide: *packing = { StereoMode::SideBySide, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoSideBySideHalf: *packing = { StereoMode::SideBySide, false, Size::Half }; return true;
    case QQuickVideoOutput::StereoSideBySideFull: *packing = { StereoMode::SideBySide, false, Size::Full }; return true;
    case QQuickVideoOutput::StereoTopBottom: *packing = { StereoMode::TopBottom, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoTopBottomHalf: *packing = { StereoMode::TopBottom, false, Size::Half }; return true;
    case QQuickVideoOutput::StereoTopBottomFull: *packing = { StereoMode::TopBottom, false, Size::Full }; return true;
    case QQuickVideoOutput::StereoSideBySideQuincunx: *packing = { StereoMode::SideBySideQuincunx, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoRowInterleaved: *packing = { StereoMode::RowInterleaved, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoColumnInterleaved: *packing = { StereoMode::ColumnInterleaved, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoCheckerboard: *packing = { StereoMode::Checkerboard, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoFrameSequence: *packing = { StereoMode::FrameSequence, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoAnaglyphCyanRed: *packing = { StereoMode::AnaglyphCyanRed, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoAnaglyphGreenMagenta: *packing = { StereoMode::AnaglyphGreenMagenta, false, Size::ByShape }; return true;
    case QQuickVideoOutput::StereoNone: *packing = {}; return true;
    case QQuickVideoOutput::StereoDepthSideBySide:
        *packing = { StereoMode::DepthSideBySide, false, Size::ByShape, false, {} };
        return true;
    case QQuickVideoOutput::StereoDepthSideBySideDepthFirst:
        *packing = { StereoMode::DepthSideBySide, false, Size::ByShape, true, {} };
        return true;
    case QQuickVideoOutput::StereoDepthTopBottom:
        *packing = { StereoMode::DepthTopBottom, false, Size::ByShape, false, {} };
        return true;
    case QQuickVideoOutput::StereoDepthTopBottomDepthFirst:
        *packing = { StereoMode::DepthTopBottom, false, Size::ByShape, true, {} };
        return true;
    case QQuickVideoOutput::StereoDepthGraphics:
        *packing = { StereoMode::DepthGraphics, false, Size::ByShape, false, {} };
        return true;
    case QQuickVideoOutput::StereoDepthBackground:
        *packing = { StereoMode::DepthBackground, false, Size::ByShape, false, {} };
        return true;
    case QQuickVideoOutput::StereoDepthSeparate:
        *packing = { StereoMode::DepthSeparate, false, Size::ByShape, false, {} };
        return true;
    case QQuickVideoOutput::StereoAutomatic: break;
    }
    return false;
}

QQuickVideoOutput::StereoInput inputFromPacking(const QQuickVideoStereo::Packing &packing, QSize frameSize)
{
    const bool full = QQuickVideoStereo::isFullSize(packing, frameSize);
    switch (packing.mode) {
    case StereoMode::SideBySide: return full ? QQuickVideoOutput::StereoSideBySideFull : QQuickVideoOutput::StereoSideBySideHalf;
    case StereoMode::TopBottom: return full ? QQuickVideoOutput::StereoTopBottomFull : QQuickVideoOutput::StereoTopBottomHalf;
    case StereoMode::SideBySideQuincunx: return QQuickVideoOutput::StereoSideBySideQuincunx;
    case StereoMode::RowInterleaved: return QQuickVideoOutput::StereoRowInterleaved;
    case StereoMode::ColumnInterleaved: return QQuickVideoOutput::StereoColumnInterleaved;
    case StereoMode::Checkerboard: return QQuickVideoOutput::StereoCheckerboard;
    case StereoMode::FrameSequence: return QQuickVideoOutput::StereoFrameSequence;
    case StereoMode::AnaglyphCyanRed: return QQuickVideoOutput::StereoAnaglyphCyanRed;
    case StereoMode::AnaglyphGreenMagenta: return QQuickVideoOutput::StereoAnaglyphGreenMagenta;
    case StereoMode::DepthSideBySide:
        return packing.depthFirst ? QQuickVideoOutput::StereoDepthSideBySideDepthFirst
                                  : QQuickVideoOutput::StereoDepthSideBySide;
    case StereoMode::DepthTopBottom:
        return packing.depthFirst ? QQuickVideoOutput::StereoDepthTopBottomDepthFirst
                                  : QQuickVideoOutput::StereoDepthTopBottom;
    case StereoMode::DepthGraphics:
        return QQuickVideoOutput::StereoDepthGraphics;
    case StereoMode::DepthBackground:
        return QQuickVideoOutput::StereoDepthBackground;
    case StereoMode::DepthSeparate:
        return QQuickVideoOutput::StereoDepthSeparate;
    case StereoMode::None: break;
    }
    return QQuickVideoOutput::StereoNone;
}

// The bits of a sample of the video, as far as its pixel format says
int sourceDepth(QVideoFrameFormat::PixelFormat format)
{
    switch (format) {
    case QVideoFrameFormat::Format_P016:
    case QVideoFrameFormat::Format_Y16:
    case QVideoFrameFormat::Format_YUV422P16:
    case QVideoFrameFormat::Format_YUV444P16:
        return 16;
    case QVideoFrameFormat::Format_P010:
    case QVideoFrameFormat::Format_YUV420P10:
        return 10;
    default:
        return 8;
    }
}

#ifdef QT_MM_STEREO_LAYER
// The node that keeps the video's layer from being covered: it writes a transparent hole where
// the video is, over what is drawn behind it. What is drawn over it, the controls, stays
class StereoHoleNode : public QSGSimpleRectNode
{
public:
    StereoHoleNode()
    {
        setColor(Qt::transparent);
        // without blending the transparent color replaces what is behind it, with its alpha
        material()->setFlag(QSGMaterial::Blending, false);
    }
};
#endif
}

/*!
    \qmltype VideoOutput
    //! \nativetype QQuickVideoOutput
    \inherits Item
    \brief Render video or camera viewfinder.

    \ingroup multimedia_qml
    \ingroup multimedia_video_qml
    \inqmlmodule QtMultimedia

    \qml

    Rectangle {
        width: 800
        height: 600
        color: "black"

        MediaPlayer {
            id: player
            source: "file://video.webm"
            videoOutput: videoOutput
        }

        VideoOutput {
            id: videoOutput
            anchors.fill: parent
        }
    }

    \endqml

    The VideoOutput item supports untransformed, stretched, and uniformly scaled video presentation.
    For a description of stretched uniformly scaled presentation, see the \l fillMode property
    description.

    \sa MediaPlayer, Camera

\omit
    \section1 Screen Saver

    If it is likely that an application will be playing video for an extended
    period of time without user interaction it may be necessary to disable
    the platform's screen saver. The \l ScreenSaver (from \l QtSystemInfo)
    may be used to disable the screensaver in this fashion:

    \qml
    import QtSystemInfo

    ScreenSaver { screenSaverEnabled: false }
    \endqml
\endomit
*/

// TODO: Restore Qt System Info docs when the module is released

/*!
    \internal
    \class QQuickVideoOutput
    \brief The QQuickVideoOutput class provides a video output item.
*/

// Frame ownership belongs to VideoOutput, not a temporary window presenter.
// Kept opaque in the header so builds without the Wayland layer retain its layout.
struct QQuickVideoOutput::StereoPair
{
#ifdef QT_MM_STEREO_LAYER
    QQuickVideoStereoPresenter::Job job;
#endif
};

QQuickVideoOutput::QQuickVideoOutput(QQuickItem *parent) :
    QQuickItem(parent)
{
    setFlag(ItemHasContents, true);

    m_sink = new QQuickVideoSink(this);
    qRegisterMetaType<QVideoFrameFormat>();

    // TODO: investigate if we have any benefit of setting frame in the source thread
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            makeGuardedCall([this](const QVideoFrame &frame) {
        if (frame.isValid() || m_endOfStreamPolicy == ClearOutput)
            setFrame(frame);
    }),
            Qt::DirectConnection);

    initRhiForSink();

#ifdef QT_MM_STEREO_LAYER
    stereoLayerSupported(); // reads the compositor's globals once, here on the GUI thread
    m_stereoTimer.setInterval(100);
    connect(&m_stereoTimer, &QTimer::timeout, this, &QQuickVideoOutput::_q_syncStereoLayer);
#endif
}

QQuickVideoOutput::~QQuickVideoOutput()
{
    {
        QMutexLocker lock(&m_destructorGuard->m_mutex);
        m_destructorGuard->m_isAlive = false;
    }

#ifdef QT_MM_STEREO_LAYER
    m_stereoTimer.stop();
#endif
    delete m_sink;
    disconnectWindowConnections();
#ifdef QT_MM_STEREO_LAYER
    m_stereoPresenter.reset();
#endif
}

/*!
    \qmlproperty object QtMultimedia::VideoOutput::videoSink

    This property holds the underlaying C++ QVideoSink object that is used
    to render the video frames to this VideoOutput element.

    Normal usage of VideoOutput from QML should not require using this property.
*/

QVideoSink *QQuickVideoOutput::videoSink() const
{
    return m_sink;
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::fillMode

    Set this property to define how the video is scaled to fit the target area.

    \list
    \li Stretch - the video is scaled to fit.
    \li PreserveAspectFit - the video is scaled uniformly to fit without cropping
    \li PreserveAspectCrop - the video is scaled uniformly to fill, cropping if necessary
    \endlist

    The default fill mode is PreserveAspectFit.
*/

QQuickVideoOutput::FillMode QQuickVideoOutput::fillMode() const
{
    return FillMode(m_aspectRatioMode);
}

void QQuickVideoOutput::setFillMode(FillMode mode)
{
    if (mode == fillMode())
        return;

    m_aspectRatioMode = Qt::AspectRatioMode(mode);

    m_geometryDirty = true;
    update();

    emit fillModeChanged(mode);
    reprocessFrame();
}

void QQuickVideoOutput::_q_newFrame(QSize size)
{
    update();

    size = qRotatedFrameSize(size, m_frameDisplayingRotation);

    if (m_nativeSize != size) {
        m_nativeSize = size;

        m_geometryDirty = true;

        setImplicitWidth(size.width());
        setImplicitHeight(size.height());

        emit sourceRectChanged();
    }
}

/* Based on fill mode and our size, figure out the source/dest rects */
void QQuickVideoOutput::_q_updateGeometry()
{
    const QRectF rect(0, 0, width(), height());
    const QRectF absoluteRect(x(), y(), width(), height());

    if (!m_geometryDirty && m_lastRect == absoluteRect)
        return;

    QRectF oldContentRect(m_contentRect);

    m_geometryDirty = false;
    m_lastRect = absoluteRect;

    const auto fill = m_aspectRatioMode;
    if (m_nativeSize.isEmpty()) {
        //this is necessary for item to receive the
        //first paint event and configure video surface.
        m_contentRect = rect;
    } else if (fill == Qt::IgnoreAspectRatio) {
        m_contentRect = rect;
    } else {
        QSizeF scaled = m_nativeSize;
        scaled.scale(rect.size(), fill);

        m_contentRect = QRectF(QPointF(), scaled);
        m_contentRect.moveCenter(rect.center());
    }

    updateGeometry();

    if (m_contentRect != oldContentRect)
        emit contentRectChanged();
}

/*!
    \qmlproperty int QtMultimedia::VideoOutput::orientation

    This property determines the angle in, degrees, at which the displayed video
    is rotated clockwise in video coordinates, where the Y-axis points
    downwards on the display.
    The orientation transformation is applied before \l mirrored.

    The orientation change affects the mapping of coordinates from the source to the viewport.

    Only multiples of \c 90 degrees are supported, that is 0, 90, -90, 180, 270, etc.,
    otherwise, the specified value is ignored.

    In some cases, the source video stream requires a certain
    orientation to be corrected. This includes
    sources like a camera viewfinder, where the displayed
    viewfinder should match the reality, no matter what rotation
    the rest of the user interface has.

    We recommend using this property to compensate a user interface
    rotation, or align the output view with other application business
    requirements.

    The default value is \c 0.
*/
int QQuickVideoOutput::orientation() const
{
    return m_orientation;
}

void QQuickVideoOutput::setOrientation(int orientation)
{
    // Make sure it's a multiple of 90.
    if (orientation % 90)
        return;

    // If there's no actual change, return
    if (m_orientation == orientation)
        return;

    // If the new orientation is the same effect
    // as the old one, don't update the video node stuff
    if (qVideoRotationFromDegrees(orientation - m_orientation) == QtVideo::Rotation::None) {
        m_orientation = orientation;
        emit orientationChanged();
        return;
    }

    m_geometryDirty = true;

    // Otherwise, a new orientation
    // See if we need to change aspect ratio orientation too
    bool oldAspect = qIsDefaultAspect(m_orientation);
    bool newAspect = qIsDefaultAspect(orientation);

    m_orientation = orientation;

    {
        QMutexLocker lock(&m_frameMutex);
        m_frameDisplayingRotation = qNormalizedFrameTransformation(m_frame, m_orientation).rotation;
    }

    if (oldAspect != newAspect) {
        m_nativeSize.transpose();

        setImplicitWidth(m_nativeSize.width());
        setImplicitHeight(m_nativeSize.height());

        // Source rectangle does not change for orientation
    }

    update();
    emit orientationChanged();
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::mirrored
    \since 6.9

    Determines whether the displayed video is mirrored around its vertical axis.
    We recommend using this property if the transformation of the source video stream,
    such as from a front camera, does not align with the application's business logic.
    The mirroring is applied after \l orientation.

    The property change affects the mapping of coordinates from the source to the viewport.

    The default value is \c false.
*/
bool QQuickVideoOutput::mirrored() const
{
    return m_mirrored;
}

void QQuickVideoOutput::setMirrored(bool mirrored)
{
    if (m_mirrored == mirrored)
        return;
    m_mirrored = mirrored;

    update();
    emit mirroredChanged();
}

/*!
    \qmlproperty rectangle QtMultimedia::VideoOutput::contentRect

    This property holds the item coordinates of the area that
    would contain video to render.  With certain fill modes,
    this rectangle will be larger than the visible area of the
    \c VideoOutput.

    This property is useful when other coordinates are specified
    in terms of the source dimensions - this applied for relative
    (normalized) frame coordinates in the range of 0 to 1.0.

    Areas outside this will be transparent.
*/
QRectF QQuickVideoOutput::contentRect() const
{
    return m_contentRect;
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::endOfStreamPolicy
    \since 6.9

    This property specifies the policy to apply when the video stream ends. This
    occurs at the end of media playback or upon deactivation of the camera, screen,
    or window capture.

    The \c endOfStreamPolicy can be one of:

    \value ClearOutput The video output is cleared.
    \value KeepLastFrame The video output continues displaying the
           last frame. Use the method \l clearOutput() to clear the output manually.

    The default value is \c ClearOutput.
*/
QQuickVideoOutput::EndOfStreamPolicy QQuickVideoOutput::endOfStreamPolicy() const
{
    return m_endOfStreamPolicy;
}

void QQuickVideoOutput::setEndOfStreamPolicy(EndOfStreamPolicy policy)
{
    if (m_endOfStreamPolicy == policy)
        return;

    m_endOfStreamPolicy = policy;
    emit endOfStreamPolicyChanged(policy);
}

/*!
    \qmlmethod void QtMultimedia::VideoOutput::clearOutput()
    \since 6.9

    Clears the video output by removing the currently displayed video frame.
    This method is recommended when you need to remove the last video frame after
    detaching the video output from the source or after the source stream ends
    with the video output's \l endOfStreamPolicy property set to \c KeepLastFrame.
*/
void QQuickVideoOutput::clearOutput()
{
    setFrame({});
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoInput

    How the frames of a stereoscopic 3D video hold its two views. \c StereoAutomatic (the
    default) takes what the stream says (the frame packing arrangement SEI of H.264 and H.265,
    Matroska's StereoMode, see QVideoFrameFormat::stereoMode()); the other values say it for a
    video that does not, or says it wrong:

    \value StereoAutomatic What the stream says.
    \value StereoSideBySide Side by side, the first view on the left; "half" or "full" by the shape
           of the frame.
    \value StereoSideBySideHalf Side by side, each view squeezed to half the width of the frame.
    \value StereoSideBySideFull Side by side, each view as wide as half the frame.
    \value StereoTopBottom Top and bottom, the first view on top; "half" or "full" by the shape of
           the frame.
    \value StereoTopBottomHalf Top and bottom, each view squeezed to half the height.
    \value StereoTopBottomFull Top and bottom, each view as high as half the frame.
    \value StereoSideBySideQuincunx Side by side with quincunx sampling.
    \value StereoRowInterleaved The views in alternate rows, the first in the first row.
    \value StereoColumnInterleaved The views in alternate columns, the first in the first column.
    \value StereoCheckerboard The views in a checkerboard, the first at the top left pixel.
    \value StereoFrameSequence The frames are the views in turn.
    \value StereoAnaglyphCyanRed Both views in the colors, red and cyan: shown as they are.
    \value StereoAnaglyphGreenMagenta Both views in the colors, green and magenta: shown as they
           are.
    \value StereoNone The video is not 3D, whatever the stream says.
    \value StereoDepthSideBySide A picture and its depth map next to each other, the picture on
           the left: the views are drawn from the depth (\l stereoDepth).
    \value StereoDepthSideBySideDepthFirst A depth map and its picture next to each other, the
           depth map on the left.
    \value StereoDepthTopBottom A picture above its depth map.
    \value StereoDepthTopBottomDepthFirst A depth map above its picture.
    \value StereoDepthGraphics Four rows from the top: the picture, its depth map, graphics and
           their depth map, where a depth sample of 0 is no graphics.
    \value StereoDepthBackground Four quarters: the picture and its depth map above a background
           and its depth map, which fills what the picture uncovers.
    \value StereoDepthSeparate The frame is a picture whose depth map is not in it: the picture
           is shown alone.

    The packings are those of H.264 Table D-8 and H.265 Table D.8, and of RFC 9559 section
    5.1.4.1.28.3 (StereoMode 0 to 14). The first view is the left eye's, unless the stream says
    the right eye comes first; \l stereoSwapEyes turns the order round. The depth layouts are
    the depth marks of Sparky Stereo OS (stereo-layer, docs/depth-marks.md): Stereo3DLayout
    depthSideBySidePictureFirst, depthSideBySideDepthFirst, depthTopBottomPictureFirst,
    depthTopBottomDepthFirst, depthGraphicsTopBottom (HDMI 1.4a, 3D_Structure 0101, "L + depth +
    graphics + graphics-depth"), depthBackgroundQuarters and depthSeparate. The picture and its
    depth map of a stream whose depth comes apart from its pictures, an H.265 depth auxiliary
    layer (AuxId 2) or an ISO/IEC 23002-3 auxiliary video stream, come packed as
    \c StereoDepthSideBySide.
*/
QQuickVideoOutput::StereoInput QQuickVideoOutput::stereoInput() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.input;
}

void QQuickVideoOutput::setStereoInput(StereoInput input)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.input == input)
            return;
        m_stereo.input = input;
    }
    emit stereoInputChanged();
    reprocessFrame();
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::stereoSwapEyes

    Whether the first and the second view of a stereoscopic 3D video are the other way round
    from what the stream says. The default value is \c false.
*/
bool QQuickVideoOutput::stereoSwapEyes() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.swapEyes;
}

void QQuickVideoOutput::setStereoSwapEyes(bool swap)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.swapEyes == swap)
            return;
        m_stereo.swapEyes = swap;
    }
    emit stereoSwapEyesChanged();
    reprocessFrame();
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoView

    What is shown of a stereoscopic 3D video:

    \value StereoBothEyes Each eye its view. Where the compositor takes stereo content, the
           views are in a surface of their own under the window's (\l stereoLayerActive), declared
           as
           full side by side, and the window's own surface keeps what it draws over the video, the
           same in both eyes. Elsewhere the left eye's view is shown.
    \value StereoLeftEye The left eye's view only.
    \value StereoRightEye The right eye's view only.
    \value StereoFlat The frame as it is stored, as if the video were not 3D.

    The default value is \c StereoBothEyes. A video that is not 3D is shown as it is.
*/
QQuickVideoOutput::StereoView QQuickVideoOutput::stereoView() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.view;
}

void QQuickVideoOutput::setStereoView(StereoView view)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.view == view)
            return;
        m_stereo.view = view;
    }
    emit stereoViewChanged();
    reprocessFrame();
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::stereoShapeFallback

    Whether a video that says nothing about being 3D is taken for full side by side when it
    is at least 3 times as wide as high: two pictures next to each other (two 16:9 pictures are
    32:9, 3.56:1), never one picture (a scope film is 2.39:1, the widest film formats stay under
    3:1). It is the last resort, after the stream's own signal and stereoInput; below 3:1 only
    stereoInput makes a video that says nothing 3D. The default value is \c true.
*/
bool QQuickVideoOutput::stereoShapeFallback() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.shapeFallback;
}

void QQuickVideoOutput::setStereoShapeFallback(bool fallback)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.shapeFallback == fallback)
            return;
        m_stereo.shapeFallback = fallback;
    }
    emit stereoShapeFallbackChanged();
    reprocessFrame();
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoScaling

    The filter that restores the full size of one view of a 3D video shown flat in the
    scene graph (\l stereoView StereoLeftEye or StereoRightEye of an interleaved packing,
    whose view has half its rows or columns). Where the views are shown in the stereo layer,
    they go to the compositor at the resolution the video carries them, a half view as it is,
    and the compositor resizes them once.

    \value StereoBilinear Bilinear.
    \value StereoBicubic Bicubic (Catmull-Rom).
    \value StereoLanczos Lanczos with three lobes.

    The default value is \c StereoLanczos.
*/
QQuickVideoOutput::StereoScaling QQuickVideoOutput::stereoScaling() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.scaling;
}

void QQuickVideoOutput::setStereoScaling(StereoScaling scaling)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.scaling == scaling)
            return;
        m_stereo.scaling = scaling;
    }
    emit stereoScalingChanged();
    reprocessFrame();
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoSurface

    The depth of the surface the two views are shown in, when it is the surface of the layer.

    \value StereoSurface8Bit 8 bits per channel.
    \value StereoSurface10Bit 10 bits per channel (the compositor takes it or the surface stays at
           8).
    \value StereoSurfaceHalfFloat Half float, or 16 bits per channel where the compositor has no
           half float.
    \value StereoSurfaceBySource The depth of the video: 8 bits for 8 bit video, 10 bits for
           10 bit video, half float above.

    The default value is \c StereoSurfaceBySource.
*/
QQuickVideoOutput::StereoSurface QQuickVideoOutput::stereoSurface() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.surface;
}

void QQuickVideoOutput::setStereoSurface(StereoSurface surface)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.surface == surface)
            return;
        m_stereo.surface = surface;
    }
    emit stereoSurfaceChanged();
    reprocessFrame();
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::stereoVideo
    \readonly

    Whether the video in hand has two views in each frame that can be told apart (as the stream or
    \l stereoInput says). The anaglyphs have both views in the colors, they are shown as they are.
*/
bool QQuickVideoOutput::stereoVideo() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.video;
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::stereoDepth

    Whether the two views of a video that comes with its depth map are drawn from it
    (depth-image-based rendering) and shown as any other 3D video. When \c false, the picture is
    shown alone. The default value is \c true.
*/
bool QQuickVideoOutput::stereoDepth() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.enabled;
}

void QQuickVideoOutput::setStereoDepth(bool value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.enabled == value)
            return;
        m_stereo.depth.enabled = value;
    }
    emit stereoDepthChanged();
    reprocessFrame();
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoDepthView

    What the picture of a video with its depth map is: \list \li \c StereoDepthPictureLeft (the
    default): the left eye's view; the right eye's is drawn \li \c StereoDepthPictureCenter: the
    middle; both eyes' views are drawn \endlist
*/
QQuickVideoOutput::StereoDepthView QQuickVideoOutput::stereoDepthView() const
{
    QMutexLocker lock(&m_frameMutex);
    return StereoDepthView(m_stereo.depth.view);
}

void QQuickVideoOutput::setStereoDepthView(StereoDepthView value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.view == int(value))
            return;
        m_stereo.depth.view = int(value);
    }
    emit stereoDepthViewChanged();
    reprocessFrame();
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoDepthWarp

    How the picture is warped into a view: \c StereoDepthForward moves each pixel of
    the picture to its place in the view, \c StereoDepthBackward warps the depth first and then
    takes each pixel of the view from the picture, \c StereoDepthLayered draws the edges of near
    objects as a layer over the rest. The default is libstereo-dibr's, the same for every
    program, and while \l stereoDepthAutomatic is \c true the way is chosen for the video:
    setting this picks the warping and turns that off.
*/
QQuickVideoOutput::StereoDepthWarp QQuickVideoOutput::stereoDepthWarp() const
{
    QMutexLocker lock(&m_frameMutex);
    return StereoDepthWarp(m_stereo.depth.warp);
}

void QQuickVideoOutput::setStereoDepthWarp(StereoDepthWarp value)
{
    // picking a warping is picking the way: the way is the program's, not the automatic one
    bool changed = false, automaticChanged = false;
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.warp != int(value)) {
            m_stereo.depth.warp = int(value);
            changed = true;
        }
        if (m_stereo.depth.automatic) {
            m_stereo.depth.automatic = false;
            automaticChanged = true;
        }
    }
    if (automaticChanged)
        emit stereoDepthAutomaticChanged();
    if (changed)
        emit stereoDepthWarpChanged();
    if (changed || automaticChanged)
        reprocessFrame();
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoDepthFill

    How the areas each view sees behind near objects, which the picture does not show, are filled:
    \c StereoDepthFillBackground extends the background, \c StereoDepthFillDepth takes
    background pixels only, \c StereoDepthFillPyramid does the same by halves of the picture, in a
    number of steps that grows with the logarithm of the hole (about the error of
    \c StereoDepthFillDepth at a seventh of its time on a GPU), \c StereoDepthFillTelea inpaints
    from the background side (Telea), \c StereoDepthFillNone leaves them black. The default is
    libstereo-dibr's, the same for every program, and while \l stereoDepthAutomatic is \c true
    the way is chosen for the video: setting this picks the fill and turns that off.
*/
QQuickVideoOutput::StereoDepthFill QQuickVideoOutput::stereoDepthFill() const
{
    QMutexLocker lock(&m_frameMutex);
    return StereoDepthFill(m_stereo.depth.fill);
}

void QQuickVideoOutput::setStereoDepthFill(StereoDepthFill value)
{
    // picking a gap filling is picking the way: the way is the program's, not the automatic one
    bool changed = false, automaticChanged = false;
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.fill != int(value)) {
            m_stereo.depth.fill = int(value);
            changed = true;
        }
        if (m_stereo.depth.automatic) {
            m_stereo.depth.automatic = false;
            automaticChanged = true;
        }
    }
    if (automaticChanged)
        emit stereoDepthAutomaticChanged();
    if (changed)
        emit stereoDepthFillChanged();
    if (changed || automaticChanged)
        reprocessFrame();
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoDepthSmooth

    Whether the depth map is smoothed before the views are drawn: \c StereoDepthSmoothOff (the
    default) or \c StereoDepthSmoothGaussian, an asymmetric Gaussian of stereoDepthSigmaH and
    stereoDepthSigmaV.
*/
QQuickVideoOutput::StereoDepthSmooth QQuickVideoOutput::stereoDepthSmooth() const
{
    QMutexLocker lock(&m_frameMutex);
    return StereoDepthSmooth(m_stereo.depth.smooth);
}

void QQuickVideoOutput::setStereoDepthSmooth(StereoDepthSmooth value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.smooth == int(value))
            return;
        m_stereo.depth.smooth = int(value);
    }
    emit stereoDepthSmoothChanged();
    reprocessFrame();
}

/*!
    \qmlproperty real QtMultimedia::VideoOutput::stereoDepthSigmaH

    The horizontal standard deviation of the smoothing, in pixels of a view 1920 wide. The default
    value is 10.
*/
qreal QQuickVideoOutput::stereoDepthSigmaH() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.sigmaH;
}

void QQuickVideoOutput::setStereoDepthSigmaH(qreal value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.sigmaH == value)
            return;
        m_stereo.depth.sigmaH = value;
    }
    emit stereoDepthSigmaHChanged();
    reprocessFrame();
}

/*!
    \qmlproperty real QtMultimedia::VideoOutput::stereoDepthSigmaV

    The vertical standard deviation of the smoothing, in pixels of a view 1920 wide. The default
    value is 30.
*/
qreal QQuickVideoOutput::stereoDepthSigmaV() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.sigmaV;
}

void QQuickVideoOutput::setStereoDepthSigmaV(qreal value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.sigmaV == value)
            return;
        m_stereo.depth.sigmaV = value;
    }
    emit stereoDepthSigmaVChanged();
    reprocessFrame();
}

/*!
    \qmlproperty int QtMultimedia::VideoOutput::stereoDepthRadius

    The neighbourhood that the StereoDepthFillDepth and StereoDepthFillTelea fills take the colours
    from, in pixels. The default value is 5.
*/
int QQuickVideoOutput::stereoDepthRadius() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.radius;
}

void QQuickVideoOutput::setStereoDepthRadius(int value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.radius == value)
            return;
        m_stereo.depth.radius = value;
    }
    emit stereoDepthRadiusChanged();
    reprocessFrame();
}

/*!
    \qmlproperty real QtMultimedia::VideoOutput::stereoDepthRange

    For depth maps without distances, the largest parallax either side of the screen, in pixels of a
    view 1920 wide. The default value is 58, about one degree of visual angle for a viewer three
    picture heights away.
*/
qreal QQuickVideoOutput::stereoDepthRange() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.range;
}

void QQuickVideoOutput::setStereoDepthRange(qreal value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.range == value)
            return;
        m_stereo.depth.range = value;
    }
    emit stereoDepthRangeChanged();
    reprocessFrame();
}

/*!
    \qmlproperty real QtMultimedia::VideoOutput::stereoDepthZero

    For depth maps without distances, where the screen is, from the farthest samples (0) to the
    nearest (1). The default value is 0.5.
*/
qreal QQuickVideoOutput::stereoDepthZero() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.zero;
}

void QQuickVideoOutput::setStereoDepthZero(qreal value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.zero == value)
            return;
        m_stereo.depth.zero = value;
    }
    emit stereoDepthZeroChanged();
    reprocessFrame();
}

/*!
    \qmlproperty real QtMultimedia::VideoOutput::stereoDepthShift

    A parallax added to every point, in pixels of a view 1920 wide: positive moves the whole scene
    behind the screen, negative in front of it. The default value is 0.
*/
qreal QQuickVideoOutput::stereoDepthShift() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.shift;
}

void QQuickVideoOutput::setStereoDepthShift(qreal value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.shift == value)
            return;
        m_stereo.depth.shift = value;
    }
    emit stereoDepthShiftChanged();
    reprocessFrame();
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoDepthMeaning

    What a sample of the depth map means, in place of what the video says (\c StereoDepthAutomatic,
    the default): \c StereoDepthNearness (larger samples nearer, 2D-plus-depth),
    \c StereoDepthFarness, \c StereoDepthInverseZ, \c StereoDepthDisparity, \c StereoDepthZ and
    \c StereoDepthNonlinearDisparity (the depth representation types 0 to 3 of H.265 Table G.3),
    \c StereoDepthParallax (an ISO/IEC 23002-3 parallax map), \c StereoDepthRangeLinear and
    \c StereoDepthRangeInverse (the depth map formats of Google's Dynamic Depth 1.0) and
    \c StereoDepthMetric (a depth sensor's distances, 0 being no measurement).
*/
QQuickVideoOutput::StereoDepthMeaning QQuickVideoOutput::stereoDepthMeaning() const
{
    QMutexLocker lock(&m_frameMutex);
    return StereoDepthMeaning(m_stereo.depth.meaning);
}

void QQuickVideoOutput::setStereoDepthMeaning(StereoDepthMeaning value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.meaning == int(value))
            return;
        m_stereo.depth.meaning = int(value);
    }
    emit stereoDepthMeaningChanged();
    reprocessFrame();
}

/*!
    \qmlproperty real QtMultimedia::VideoOutput::stereoDepthNear

    With stereoDepthFar: ZNear of a Z depth map that carries none, Near of a Dynamic Depth map, and
    the distance the inverse of a sensor's distances is spread from, in the map's unit. Used when
    both are above 0. The default value is 0.
*/
qreal QQuickVideoOutput::stereoDepthNear() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.zNear;
}

void QQuickVideoOutput::setStereoDepthNear(qreal value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.zNear == value)
            return;
        m_stereo.depth.zNear = value;
    }
    emit stereoDepthNearChanged();
    reprocessFrame();
}

/*!
    \qmlproperty real QtMultimedia::VideoOutput::stereoDepthFar

    With stereoDepthNear: ZFar, Far, and the distance the inverse of a sensor's distances is spread
    to. The default value is 0.
*/
qreal QQuickVideoOutput::stereoDepthFar() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.zFar;
}

void QQuickVideoOutput::setStereoDepthFar(qreal value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.zFar == value)
            return;
        m_stereo.depth.zFar = value;
    }
    emit stereoDepthFarChanged();
    reprocessFrame();
}

/*!
    \qmlproperty string QtMultimedia::VideoOutput::stereoDepthMark

    What a depth sample means as a Stereo3DDepth mark of Sparky Stereo OS's depth marks
    (stereo-layer, docs/depth-marks.md): a word for the kind, then key=value pairs separated by
    spaces, such as \c{"nonlinearDisparity min=2 max=40 width=1920 model=0,20,7,0"}. It gives every
    parameter of every kind, and comes before stereoDepthMeaning and what the video says;
    stereoDepthRange, stereoDepthZero and stereoDepthShift apply where it has none. A malformed mark
    shows the picture alone. The default value is empty.
*/
QString QQuickVideoOutput::stereoDepthMark() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.mark;
}

void QQuickVideoOutput::setStereoDepthMark(const QString &value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.mark == value)
            return;
        m_stereo.depth.mark = value;
    }
    emit stereoDepthMarkChanged();
    reprocessFrame();
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::stereoDepthAutomatic

    Whether the way the views are drawn (the warping and the gap filling) is chosen for the
    video: the best way that keeps the frame rate of the video on the GPU that draws it, measured
    once for each GPU and picture size and kept in the person's cache
    (\c XDG_CACHE_HOME/stereo-dibr/timings, never in the media file). \l stereoDepthChoice says
    what was chosen and why. The default value is \c true. Setting \l stereoDepthWarp or
    \l stereoDepthFill picks the way and turns it off: the pick then holds for the video, the
    slowest way included. Setting it \c true gives the way back to the choice.
*/
bool QQuickVideoOutput::stereoDepthAutomatic() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.automatic;
}

void QQuickVideoOutput::setStereoDepthAutomatic(bool value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.automatic == value)
            return;
        m_stereo.depth.automatic = value;
    }
    emit stereoDepthAutomaticChanged();
    reprocessFrame();
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::stereoDepthGpu

    Whether the views are drawn on the GPU when the frame can be (OpenGL, libstereo-dibr's GLSL
    passes, in a context on the GPU the process draws with); \c false draws them with the C code
    on the processor. The processor also draws the frames the GPU does not read, 10 bit and
    16 bit video among them, and when no OpenGL context can be made: the log of
    \c qt.multimedia.video.depth says so, with the reason. The default value is \c true.
*/
bool QQuickVideoOutput::stereoDepthGpu() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.depth.gpu;
}

void QQuickVideoOutput::setStereoDepthGpu(bool value)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_stereo.depth.gpu == value)
            return;
        m_stereo.depth.gpu = value;
    }
    emit stereoDepthGpuChanged();
    reprocessFrame();
}

/*!
    \qmlproperty var QtMultimedia::VideoOutput::stereoDepthChoice

    What the automatic way chose for the video that plays: a map with \c warp and \c fill (the
    values of \l stereoDepthWarp and \l stereoDepthFill), \c keepsUp (whether it keeps the frame
    rate), \c fps, \c ms and \c budgetMs (its slowest frames' time and the time a frame may
    take), \c skippedWarp and \c skippedFill (the best way that does not keep up, -1 for none),
    \c skippedMs, \c onGpu and \c gpu (the GPU's or the processor's name). Empty when no video
    that carries depth is drawn or the way is the program's.
*/
QVariantMap QQuickVideoOutput::stereoDepthChoice() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_depthChoice;
}

/*!
    \qmlproperty var QtMultimedia::VideoOutput::stereoDepthGpuMemory

    What the GPU that draws the views of a depth video has free, for the controls to say beside a way that would not fit: a map
    with \c freeBytes (what the driver says is free, -1 when it does not say), \c width and \c height (a view's size) and \c bits
    (the depth of the video). Empty when the processor draws. A way picked by the person runs even when it would not fit.
*/
QVariantMap QQuickVideoOutput::stereoDepthGpuMemory() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_depthGpuMemory;
}

void QQuickVideoOutput::setDepthGpuMemory(const QQuickVideoDepth::GpuMemory &memory)
{
    QVariantMap map;
    if (memory.valid) {
        map.insert(QStringLiteral("freeBytes"), memory.freeBytes);
        map.insert(QStringLiteral("width"), memory.width);
        map.insert(QStringLiteral("height"), memory.height);
        map.insert(QStringLiteral("bits"), memory.bits);
    }
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_depthGpuMemory == map)
            return;
        m_depthGpuMemory = map;
    }
    QMetaObject::invokeMethod(this, &QQuickVideoOutput::stereoDepthGpuMemoryChanged, Qt::QueuedConnection);
}

/*!
    \qmlproperty var QtMultimedia::VideoOutput::stereoDepthRoute

    How the frames of a video that carries depth travel while its views are drawn on the GPU: a map with \c frames (\c memory for
    a frame in system memory, \c imported for a decoder's hardware frame the passes read where it is, \c copied for one read back to
    system memory) and \c framesWhy (the reason for a copy), \c views (\c texture for views handed to the video sink as GPU
    textures, \c readBack for views read back to system memory, \c memory for views drawn by the processor) and \c viewsWhy, and
    \c layer when the views are shown in the compositor's stereo layer, whose presenter takes them from system memory (\c views is then
    \c readBack). Empty when no views are drawn. \l stereoDepthReadBacks() counts the read backs.
*/
QVariantMap QQuickVideoOutput::stereoDepthRoute() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_depthRoute;
}

QVariantMap QQuickVideoOutput::stereoDepthReadBacks() const
{
    QMutexLocker lock(&m_depthMutex);
    QVariantMap map;
    if (m_depthRenderer) {
        map.insert(QStringLiteral("frames"), m_depthRenderer->hardwareReadBacks());
        map.insert(QStringLiteral("views"), m_depthRenderer->viewsReadBacks());
        int empty = 0;
        map.insert(QStringLiteral("importChecks"), m_depthRenderer->importChecks(&empty));
        map.insert(QStringLiteral("importEmpty"), empty);
#ifdef QT_MM_STEREO_LAYER
        {
            QMutexLocker frameLock(&m_frameMutex);
            if (m_stereoPresenter) {
                const auto route = m_stereoPresenter->layerRoute();
                map.insert(QStringLiteral("layerGpuFrames"), route.gpuFrames);
                map.insert(QStringLiteral("layerCopiedFrames"), route.copiedFrames);
            }
        }
#endif
    }
    return map;
}

void QQuickVideoOutput::setDepthRoute(const QVariantMap &route)
{
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_depthRoute == route)
            return;
        m_depthRoute = route;
    }
    QMetaObject::invokeMethod(this, &QQuickVideoOutput::stereoDepthRouteChanged, Qt::QueuedConnection);
}

void QQuickVideoOutput::setDepthChoice(const QQuickVideoDepth::Choice &choice)
{
    QVariantMap map;
    if (choice.valid) {
        map.insert(QStringLiteral("warp"), choice.warp);
        map.insert(QStringLiteral("fill"), choice.fill);
        map.insert(QStringLiteral("keepsUp"), choice.keepsUp);
        map.insert(QStringLiteral("fps"), choice.fps);
        map.insert(QStringLiteral("ms"), choice.ms);
        map.insert(QStringLiteral("budgetMs"), choice.budgetMs);
        map.insert(QStringLiteral("skippedWarp"), choice.skippedWarp);
        map.insert(QStringLiteral("skippedFill"), choice.skippedFill);
        map.insert(QStringLiteral("skippedMs"), choice.skippedMs);
        map.insert(QStringLiteral("onGpu"), choice.onGpu);
        map.insert(QStringLiteral("gpuLost"), choice.gpuLost);
        map.insert(QStringLiteral("gpu"), choice.gpu);
    }
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_depthChoice == map)
            return;
        m_depthChoice = map;
    }
    // from the thread of the video: the item's signal goes out from the item's thread
    QMetaObject::invokeMethod(this, &QQuickVideoOutput::stereoDepthChoiceChanged,
                              Qt::QueuedConnection);
}

/*!
    \qmlproperty enumeration QtMultimedia::VideoOutput::stereoPacking
    \readonly

    The packing of the video in hand, as a \l stereoInput value (never \c StereoAutomatic; side
    by side and top and bottom with "half" or "full" as the shape of the frame has it).
    \c StereoNone for a video that is not 3D.
*/
QQuickVideoOutput::StereoInput QQuickVideoOutput::stereoPacking() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.packing;
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::stereoLayerSupported
    \readonly

    Whether the compositor takes the views as stereo content in a surface of their own
    (wl_subcompositor, wp_viewporter and the stereo content declaration), and the window has the
    alpha buffer the layer needs.
*/
bool QQuickVideoOutput::stereoLayerSupported()
{
#ifdef QT_MM_STEREO_LAYER
    return QQuickVideoStereoLayer::isSupported();
#else
    return false;
#endif
}

/*!
    \qmlproperty bool QtMultimedia::VideoOutput::stereoLayerActive
    \readonly

    Whether the video is shown through the layer: both views, each to its eye. The item itself
    is then a transparent hole in the window over the layer.
*/
bool QQuickVideoOutput::stereoLayerActive() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_stereo.layerActive;
}

void QQuickVideoOutput::reprocessFrame()
{
    QVideoFrame frame;
    {
        QMutexLocker lock(&m_frameMutex);
        frame = m_stereoLastFrame;
    }
    if (frame.isValid())
        setFrame(frame, true);
}

void QQuickVideoOutput::ensureStereoPresenter()
{
#ifdef QT_MM_STEREO_LAYER
    if (m_stereoPresenter || !m_window || !m_window->handle() || !stereoLayerSupported())
        return;
    if (!m_window->format().hasAlpha()) {
        qCWarning(qLcVideo) << "stereo layer: the window has no alpha buffer";
        return;
    }
    auto presenter = std::make_shared<QQuickVideoStereoPresenter>(m_window);
    if (!presenter->isValid())
        return;
    const QPoint margins(m_window->frameMargins().left(), m_window->frameMargins().top());
    m_stereoRect = mapRectToScene(fillMode() == PreserveAspectCrop ? boundingRect() : m_contentRect).toRect().translated(margins);
    presenter->setGeometry({ m_stereoRect, fillMode() == PreserveAspectCrop });
    {
        QMutexLocker lock(&m_frameMutex);
        m_stereoPresenter = std::move(presenter);
        m_stereoPresenterReady = true;
    }
    reprocessFrame();
#endif
}

void QQuickVideoOutput::_q_syncStereoLayer()
{
#ifdef QT_MM_STEREO_LAYER
    const bool cropGeometrySupported = QQuickVideoStereo::canCropLayer(this);
    bool geometryChanged;
    {
        QMutexLocker lock(&m_frameMutex);
        geometryChanged = m_stereo.cropGeometrySupported != cropGeometrySupported;
        m_stereo.cropGeometrySupported = cropGeometrySupported;
    }
    if (geometryChanged)
        reprocessFrame();
    bool wanted;
    {
        QMutexLocker lock(&m_frameMutex);
        wanted = m_stereo.layerWanted;
    }
    if (!wanted) {
        if (m_stereoPresenter) {
            std::shared_ptr<QQuickVideoStereoPresenter> old;
            {
                QMutexLocker lock(&m_frameMutex);
                m_stereoPresenterReady = false;
                old = std::move(m_stereoPresenter);
            }
        }
        return;
    }
    if (!m_stereoPresenter) {
        ensureStereoPresenter();
        return;
    }
    bool hidden = !m_window || !isVisible() || !m_window->isVisible();
    for (QQuickItem *item = this; item; item = item->parentItem())
        hidden = hidden || item->opacity() <= 0;
    if (hidden) {
        m_stereoPresenter->hide();
        m_stereoRect = {};
        return;
    }
    // The surface of the window holds its frame, if the window has one
    const QPoint margins(m_window->frameMargins().left(), m_window->frameMargins().top());
    const QRect rect = mapRectToScene(fillMode() == PreserveAspectCrop ? boundingRect() : m_contentRect).toRect().translated(margins);
    m_stereoRect = rect;
    m_stereoPresenter->setGeometry({ rect, fillMode() == PreserveAspectCrop });
#endif
}

/*!
    \qmlproperty rectangle QtMultimedia::VideoOutput::sourceRect

    This property holds the area of the source video
    content that is considered for rendering.  The
    values are in source pixel coordinates, adjusted for
    the source's pixel aspect ratio.

    Note that typically the top left corner of this rectangle
    will be \c {0,0} while the width and height will be the
    width and height of the input content. Only when the video
    source has a viewport set, these values will differ.

    The orientation setting does not affect this rectangle.

    \sa QVideoFrameFormat::viewport()
*/
QRectF QQuickVideoOutput::sourceRect() const
{
    // We might have to transpose back
    QSizeF size = m_nativeSize;
    if (!size.isValid())
        return {};

    if (!qIsDefaultAspect(m_frameDisplayingRotation))
        size.transpose();


    // Take the viewport into account for the top left position.
    // m_nativeSize is already adjusted to the viewport, as it originates
    // from QVideoFrameFormat::viewport(), which includes pixel aspect ratio
    const QRectF viewport = adjustedViewport();
    Q_ASSERT(viewport.size() == size);
    return QRectF(viewport.topLeft(), size);
}

void QQuickVideoOutput::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    Q_UNUSED(newGeometry);
    Q_UNUSED(oldGeometry);

    QQuickItem::geometryChange(newGeometry, oldGeometry);

    // Explicitly listen to geometry changes here. This is needed since changing the position does
    // not trigger a call to updatePaintNode().
    // We need to react to position changes though, as the window backened's display rect gets
    // changed in that situation.
    _q_updateGeometry();
}

void QQuickVideoOutput::releaseResources()
{
    // Called on the gui thread when the window is closed or changed.
    initRhiForSink();
    QQuickItem::releaseResources();
}

void QQuickVideoOutput::initRhiForSink()
{
    QRhi *rhi = m_window ? QQuickWindowPrivate::get(m_window)->rhi : nullptr;
    m_sink->setRhi(rhi);
}

void QQuickVideoOutput::itemChange(QQuickItem::ItemChange change,
                                    const QQuickItem::ItemChangeData &changeData)
{
    if (change != QQuickItem::ItemSceneChange)
        return;

    if (changeData.window == m_window)
        return;

    disconnectWindowConnections();
    m_window = changeData.window;

#ifdef QT_MM_STEREO_LAYER
    {
        // the layer belongs to a window
        m_stereoTimer.stop();
        std::shared_ptr<QQuickVideoStereoPresenter> old;
        QMutexLocker lock(&m_frameMutex);
        m_stereoPresenterReady = false;
        old = std::move(m_stereoPresenter);
        lock.unlock();
    }
#endif
    if (m_window) {
#ifdef QT_MM_STEREO_LAYER
        // The layer under the window needs the window to have an alpha buffer, which it can
        // only be given before it is created
        if (stereoLayerSupported() && !m_window->handle()) {
            QSurfaceFormat format = m_window->format();
            if (format.alphaBufferSize() < 8) {
                format.setAlphaBufferSize(8);
                m_window->setFormat(format);
            }
        }
        if (stereoLayerSupported())
            m_stereoTimer.start();
#endif

        auto connectToWindow = [&](auto signal, auto function) {
            connect(m_window, signal, this, makeGuardedCall(std::move(function)),
                    Qt::DirectConnection);
        };

        // We want to receive the signals in the render thread
        connectToWindow(&QQuickWindow::sceneGraphInitialized, [this] {
            initRhiForSink();
        });

        connectToWindow(&QQuickWindow::sceneGraphInvalidated, [this] {
            if (auto texturePool = m_texturePool.lock())
                texturePool->clearTextures();
            m_sink->setRhi(nullptr);
        });

        connectToWindow(&QQuickWindow::afterFrameEnd, [this] {
            if (auto texturePool = m_texturePool.lock())
                texturePool->onFrameEndInvoked();
        });
    }
    initRhiForSink();
}

QSize QQuickVideoOutput::nativeSize() const
{
    return m_videoFormat.viewport().size();
}

void QQuickVideoOutput::updateGeometry()
{
    const QRectF viewport = m_videoFormat.viewport();
    const QSizeF frameSize = m_videoFormat.frameSize();
    QRectF normalizedViewport(viewport.x() / frameSize.width(),
                              viewport.y() / frameSize.height(),
                              viewport.width() / frameSize.width(),
                              viewport.height() / frameSize.height());
    // one view of a stereoscopic 3D video is a part of the viewport
    if (m_stereo.eyeRect != QRectF(0, 0, 1, 1)) {
        const QRectF &eye = m_stereo.eyeRect;
        normalizedViewport = QRectF(normalizedViewport.x() + eye.x() * normalizedViewport.width(),
                                    normalizedViewport.y() + eye.y() * normalizedViewport.height(),
                                    eye.width() * normalizedViewport.width(),
                                    eye.height() * normalizedViewport.height());
    }
    const QRectF rect(0, 0, width(), height());
    if (nativeSize().isEmpty()) {
        m_renderedRect = rect;
        m_sourceTextureRect = normalizedViewport;
    } else if (m_aspectRatioMode == Qt::IgnoreAspectRatio) {
        m_renderedRect = rect;
        m_sourceTextureRect = normalizedViewport;
    } else if (m_aspectRatioMode == Qt::KeepAspectRatio) {
        m_sourceTextureRect = normalizedViewport;
        m_renderedRect = contentRect();
    } else if (m_aspectRatioMode == Qt::KeepAspectRatioByExpanding) {
        m_renderedRect = rect;
        const qreal contentHeight = contentRect().height();
        const qreal contentWidth = contentRect().width();

        // Calculate the size of the source rectangle without taking the viewport into account
        const qreal relativeOffsetLeft = -contentRect().left() / contentWidth;
        const qreal relativeOffsetTop = -contentRect().top() / contentHeight;
        const qreal relativeWidth = rect.width() / contentWidth;
        const qreal relativeHeight = rect.height() / contentHeight;

        // Now take the viewport size into account
        const qreal totalOffsetLeft = normalizedViewport.x() + relativeOffsetLeft * normalizedViewport.width();
        const qreal totalOffsetTop = normalizedViewport.y() + relativeOffsetTop * normalizedViewport.height();
        const qreal totalWidth = normalizedViewport.width() * relativeWidth;
        const qreal totalHeight = normalizedViewport.height() * relativeHeight;

        if (qIsDefaultAspect(m_frameDisplayingRotation)) {
            m_sourceTextureRect = QRectF(totalOffsetLeft, totalOffsetTop,
                                         totalWidth, totalHeight);
        } else {
            m_sourceTextureRect = QRectF(totalOffsetTop, totalOffsetLeft,
                                         totalHeight, totalWidth);
        }
    }
}

QSGNode *QQuickVideoOutput::updatePaintNode(QSGNode *oldNode,
                                             QQuickItem::UpdatePaintNodeData *data)
{
    Q_UNUSED(data);
    _q_updateGeometry();

    QMutexLocker lock(&m_frameMutex);

#ifdef QT_MM_STEREO_LAYER
    if (m_stereo.layerActive) {
        // The video is in the layer under the window: this item is a hole in what is drawn
        // behind it, and what is drawn over it is the window's own
        auto *hole = dynamic_cast<StereoHoleNode *>(oldNode);
        if (!hole) {
            delete oldNode;
            hole = new StereoHoleNode;
        }
        hole->setRect(fillMode() == PreserveAspectCrop ? boundingRect() : m_contentRect);
        m_frameChanged = false;
        m_frame = QVideoFrame();
        return hole;
    }
    if (oldNode && dynamic_cast<StereoHoleNode *>(oldNode)) {
        delete oldNode;
        oldNode = nullptr;
    }
#endif

    QSGVideoNode *videoNode = static_cast<QSGVideoNode *>(oldNode);

    if (m_frameChanged) {
        if (videoNode && videoNode->pixelFormat() != m_frame.pixelFormat()) {
            qCDebug(qLcVideo) << "updatePaintNode: deleting old video node because frame format changed";
            delete videoNode;
            videoNode = nullptr;
        }

        if (!m_frame.isValid()) {
            qCDebug(qLcVideo) << "updatePaintNode: no frames yet";
            m_frameChanged = false;
            return nullptr;
        }

        if (!videoNode) {
            // Get a node that supports our frame. The surface is irrelevant, our
            // QSGVideoItemSurface supports (logically) anything.
            updateGeometry();
            QRhi *rhi = m_window ? QQuickWindowPrivate::get(m_window)->rhi : nullptr;
            videoNode = new QSGVideoNode(this, m_videoFormat, rhi);
            m_texturePool = videoNode->texturePool();
            qCDebug(qLcVideo) << "updatePaintNode: Video node created. Handle type:" << m_frame.handleType();
        }
    }

    if (!videoNode) {
        m_frameChanged = false;
        m_frame = QVideoFrame();
        return nullptr;
    }

    if (m_frameChanged) {
        videoNode->setCurrentFrame(m_frame);

        updateHdr(videoNode);

        //don't keep the frame for more than really necessary
        m_frameChanged = false;
        m_frame = QVideoFrame();
    }

    videoNode->setTexturedRectGeometry(
            m_renderedRect, m_sourceTextureRect,
            VideoTransformation{ qVideoRotationFromDegrees(orientation()), m_mirrored });

    return videoNode;
}

void QQuickVideoOutput::updateHdr(QSGVideoNode *videoNode)
{
    auto *videoOutputWindow = window();
    if (!videoOutputWindow)
        return;

    auto *swapChain = videoOutputWindow->swapChain();
    if (!swapChain)
        return;

    const auto requiredSwapChainFormat = qGetRequiredSwapChainFormat(m_frame.surfaceFormat());
    if (qShouldUpdateSwapChainFormat(swapChain, requiredSwapChainFormat)) {
        auto *recreateSwapChainJob = QRunnable::create([swapChain, requiredSwapChainFormat]() {
            swapChain->destroy();
            swapChain->setFormat(requiredSwapChainFormat);
            swapChain->createOrResize();
        });

        // Even though the 'recreate swap chain' job is scheduled for the current frame the
        // effect will be visible only starting from the next frame since the recreation would
        // happen after the actual swap.
        videoOutputWindow->scheduleRenderJob(recreateSwapChainJob, QQuickWindow::AfterSwapStage);
    }

    videoNode->setSurfaceFormat(swapChain->format());
    videoNode->setHdrInfo(swapChain->hdrInfo());
}

void QQuickVideoOutput::disconnectWindowConnections()
{
    if (!m_window)
        return;

    m_window->disconnect(this);
}

QRectF QQuickVideoOutput::adjustedViewport() const
{
    return m_videoFormat.viewport();
}

void QQuickVideoOutput::setFrame(const QVideoFrame &frame, bool repeated)
{
    using namespace QQuickVideoStereo;
    using Size = Packing::Size;

    QVideoFrame shown;
    QSize displaySize = frame.size();
    StereoState st;
#ifdef QT_MM_STEREO_LAYER
    bool presenterReady = false;
#endif
    int orientation = 0;
    bool mirrored = false;
    bool cropped = false;
    {
        QMutexLocker lock(&m_frameMutex);
        st = m_stereo;
#ifdef QT_MM_STEREO_LAYER
        presenterReady = m_stereoPresenterReady;
#endif
        orientation = m_orientation;
        mirrored = m_mirrored;
        cropped = m_aspectRatioMode == Qt::KeepAspectRatioByExpanding;
    }

    // A picture with its depth map (2D plus depth): its views are drawn into full side by side
    // (QQuickVideoDepth, libstereo-dibr), which is shown from here on as any other 3D frame; the
    // frame as stored for StereoFlat; the picture alone where the views are not drawn (stereoDepth
    // false, a malformed Stereo3DDepth mark, a depth map the frame does not hold)
    QVideoFrame input = frame;
    StereoInput depthInput = StereoNone;
    QRectF depthPictureRect;
    QQuickVideoDepth::Choice depthChoice;
    QQuickVideoDepth::GpuMemory depthMemory;
    QVariantMap depthRoute;
    if (frame.isValid()) {
        const QVideoFrameFormat format = frame.surfaceFormat();
        Packing depth;
        depth.mode = format.stereoMode();
        depth.depthFirst = format.stereoFlags().testFlag(QVideoFrameFormat::StereoFlag::DepthFirst);
        if (st.input != StereoAutomatic)
            packingFromInput(st.input, &depth);
        if (QQuickVideoDepth::isDepthLayout(depth.mode)) {
            depthInput = inputFromPacking(depth, frame.size());
            bool alone = st.view != StereoFlat;
            if (alone && st.depth.enabled && depth.mode != StereoMode::DepthSeparate) {
                QVideoFrame drawn;
                QMutexLocker lock(&m_depthMutex);
                if (!m_depthRenderer)
                    m_depthRenderer = std::make_unique<QQuickVideoDepth::Renderer>();
                // the views are handed to the sink as textures when it draws with OpenGL: its RHI is the scene graph's
                m_depthRenderer->setSinkRhi(m_window ? QQuickWindowPrivate::get(m_window)->rhi : nullptr);
                const auto drawResult =
                        m_depthRenderer->draw(frame, depth.mode, depth.depthFirst,
                                              format.stereoDepthMark(), st.depth, &drawn);
                if (drawResult == QQuickVideoDepth::Renderer::Result::Drawn) {
                    input = drawn;
                    alone = false;
                    depthChoice = m_depthRenderer->choice();
                    depthMemory = m_depthRenderer->gpuMemory();
                    if (m_depthRenderer->lastOnGpu()) {
                        using FrameRoute = QQuickVideoDepth::Renderer::FrameRoute;
                        using ViewsRoute = QQuickVideoDepth::Renderer::ViewsRoute;
                        const FrameRoute fr = m_depthRenderer->lastFrameRoute();
                        const ViewsRoute vr = m_depthRenderer->lastViewsRoute();
                        depthRoute.insert(QStringLiteral("frames"), fr == FrameRoute::Imported ? QStringLiteral("imported")
                                                  : fr == FrameRoute::Copied ? QStringLiteral("copied") : QStringLiteral("memory"));
                        depthRoute.insert(QStringLiteral("framesWhy"), m_depthRenderer->lastFrameRouteWhy());
                        depthRoute.insert(QStringLiteral("views"), vr == ViewsRoute::Texture ? QStringLiteral("texture")
                                                  : vr == ViewsRoute::ReadBack ? QStringLiteral("readBack") : QStringLiteral("memory"));
                        depthRoute.insert(QStringLiteral("viewsWhy"), m_depthRenderer->lastViewsRouteWhy());
                    }
                } else if (drawResult == QQuickVideoDepth::Renderer::Result::Failed) {
                    qCDebug(qLcVideo)
                            << "the views of a picture with its depth map could not be drawn";
                }
            }
            if (alone) {
                const QSize size = frame.size();
                const QRect rect =
                        QQuickVideoDepth::pictureRect(depth.mode, depth.depthFirst, size);
                depthPictureRect = QRectF(
                        qreal(rect.x()) / size.width(), qreal(rect.y()) / size.height(),
                        qreal(rect.width()) / size.width(), qreal(rect.height()) / size.height());
            }
        }
    }
    setDepthChoice(depthChoice);
    setDepthGpuMemory(depthMemory);
    const bool depthDrawn =
            depthInput != StereoNone && depthPictureRect.isNull() && st.view != StereoFlat;
    shown = input;

    // Stereoscopic 3D: which packing the frames have, and what is to be shown of it
    Packing packing;
    bool twoViews = false;
    bool baseOnly = false;
    bool skip = false;
    int eye = 0;
    StereoState result = st;
    result.eyeRect = QRectF(0, 0, 1, 1);
    result.layerActive = false;
    result.layerWanted = false;
    if (input.isValid()) {
        const QVideoFrameFormat format = input.surfaceFormat();
        const QSize storedSize = input.size();
        // a stereo frame the backend left at its stored size is shown at the shape of its pixels
        const QSize par = format.pixelAspectRatio();
        const QSize frameSize = par.width() > 0 && par.height() > 0 && par.width() != par.height()
                ? qCalculateFrameSize(storedSize, { par.width(), par.height() })
                : storedSize;
        displaySize = frameSize;
        packing.mode = format.stereoMode();
        packing.rightEyeFirst = format.stereoFlags().testFlag(QVideoFrameFormat::StereoFlag::RightEyeFirst);
        const bool multiview = format.stereoFlags().testFlag(QVideoFrameFormat::StereoFlag::Multiview);
        baseOnly = multiview && (st.input == StereoNone || st.view == StereoFlat);
        if (multiview)
            packing.size = Size::Full;
        if (depthDrawn) {
            // the drawn views, full side by side, the left eye first, whatever the input says
            packing = { StereoMode::SideBySide, false, Size::Full, false, {} };
        } else if (st.input != StereoAutomatic && !baseOnly) {
            packingFromInput(st.input, &packing);
        }
#ifdef QT_MM_STEREO_POLICY
        else if (packing.mode == StereoMode::None && st.shapeFallback && !frameSize.isEmpty()
                 && stereo_policy_video_side_by_side_by_shape(frameSize.width(), frameSize.height())
                         == 1) {
            packing = { StereoMode::SideBySide, false, Size::Full };
        }
#endif
        if (storedSize != frameSize)
            packing.displaySize = frameSize;
        twoViews = hasTwoViews(packing.mode) && !frameSize.isEmpty();
        result.video = twoViews && !baseOnly;
        result.packing = packing.mode == StereoMode::None ? StereoNone
                                                          : inputFromPacking(packing, frameSize);
        if (!twoViews)
            result.sequence = 0;

#ifdef QT_MM_STEREO_LAYER
        if (!repeated) {
            QMutexLocker lock(&m_frameMutex);
            auto previous = m_stereoLastFrame.surfaceFormat();
            auto incoming = format;
            previous.setStereoView(QVideoFrameFormat::StereoView::Packed);
            incoming.setStereoView(QVideoFrameFormat::StereoView::Packed);
            if (m_stereoLastFrame.isValid()
                    && (previous != incoming || result.packing != st.packing)) {
                ++m_stereoSourceSerial;
                m_stereoPair.reset();
                m_stereoHeldFrame = {};
                m_stereoHeldEye = -1;
                st.sequence = result.sequence = 0;
                st.lastView = result.lastView = -1;
                st.viewVaries = result.viewVaries = false;
                if (m_stereoPresenter)
                    m_stereoPresenter->clear();
            }
        }
#endif
        // the frame shown: the views drawn from a depth map are drawn the right way up
        const auto transformation = qNormalizedFrameTransformation(input, orientation);
        // The presenter crops both native views; frame rotation and mirroring still use the scene graph.
        const bool plain = transformation.rotation == QtVideo::Rotation::None
                && !transformation.mirroredHorizontallyAfterRotation && !mirrored
                && (!cropped || st.cropGeometrySupported);
        const bool rightFirst = packing.rightEyeFirst != (baseOnly ? false : st.swapEyes);
        if (twoViews && packing.mode == StereoMode::FrameSequence) {
            // The eye this frame is for. Frames that say it, and say it differently from one
            // frame to the next, are trusted; a stream that says one view for all of them (the stream's
            // own tag) does not tell which frame is whose, so they come in turns, the first
            // frame the first eye
            const auto view = format.stereoView();
            if (!repeated && view != QVideoFrameFormat::StereoView::Packed) {
                if (st.lastView >= 0 && st.lastView != int(view))
                    result.viewVaries = true;
                result.lastView = int(view);
            }
            if (view != QVideoFrameFormat::StereoView::Packed && st.input == StereoAutomatic && result.viewVaries)
                eye = (view == QVideoFrameFormat::StereoView::Right) != st.swapEyes ? 1 : 0;
            else
                // a frame shown again is the one that was counted last
                eye = ((st.sequence - (repeated ? 1 : 0)) % 2 == 0) == rightFirst ? 1 : 0;
            if (!repeated)
                ++result.sequence;
        } else {
            result.lastView = -1;
            result.viewVaries = false;
        }

        bool shownInLayer = false;
#ifdef QT_MM_STEREO_LAYER
        // a video whose colours the compositor cannot be told (HLG without HLG, say) is better
        // shown by the scene graph, which maps them to what the window shows
        result.layerWanted = twoViews && !baseOnly && st.view == StereoBothEyes && plain && stereoLayerSupported()
                && QQuickVideoStereoLayer::canDescribe({ format.colorSpace(), format.colorTransfer(),
                                                         format.colorRange(), format.maxLuminance() });
        result.layerActive = result.layerWanted && presenterReady;
        shownInLayer = result.layerActive;
        bool resolved = twoViews && !baseOnly;
        QVideoFrame otherFrame;
        QQuickVideoStereoPresenter::Job retained;
        if (resolved && packing.mode == StereoMode::FrameSequence && !multiview) {
            QMutexLocker lock(&m_frameMutex);
            if (repeated) {
                resolved = m_stereoPair && m_stereoPair->job.otherFrame.isValid();
                if (resolved)
                    retained = m_stereoPair->job;
            } else if (m_stereoHeldFrame.isValid() && m_stereoHeldEye != (eye ^ int(st.swapEyes))) {
                otherFrame = m_stereoHeldFrame;
                m_stereoHeldFrame = {};
                m_stereoHeldEye = -1;
            } else {
                if (eye == (rightFirst ? 1 : 0) || m_stereoHeldFrame.isValid()) {
                    m_stereoHeldFrame = input;
                    m_stereoHeldEye = eye ^ int(st.swapEyes);
                }
                resolved = false;
            }
        } else if (packing.mode != StereoMode::FrameSequence || multiview) {
            QMutexLocker lock(&m_frameMutex);
            m_stereoHeldFrame = {};
            m_stereoHeldEye = -1;
        }
        if (resolved) {
            QQuickVideoStereoPresenter::Job job;
            job.frame = retained.frame.isValid() ? retained.frame : input;
            job.otherFrame = retained.frame.isValid() ? retained.otherFrame : otherFrame;
            job.packing = packing;
            job.swapEyes = st.swapEyes;
            job.scaling = st.scaling == StereoLanczos ? Scaling::Lanczos
                    : st.scaling == StereoBicubic ? Scaling::Bicubic : Scaling::Bilinear;
            const int depth = sourceDepth(format.pixelFormat());
            const StereoSurface surface = st.surface != StereoSurfaceBySource ? st.surface
                    : depth <= 8 ? StereoSurface8Bit : depth <= 10 ? StereoSurface10Bit : StereoSurfaceHalfFloat;
            job.format = surface == StereoSurface8Bit ? BufferFormat::Xrgb8888
                    : surface == StereoSurface10Bit ? BufferFormat::Xrgb2101010
                                                    : BufferFormat::Xbgr16161616f;
            job.wide = depth > 12;
            job.frameView = retained.frame.isValid()
                    ? retained.frameView ^ int(retained.swapEyes != st.swapEyes) : eye;
            job.displaySize = viewSize(packing, frameSize);
            if (result.layerActive)
                displaySize = job.displaySize;
            QMutexLocker lock(&m_frameMutex);
            job.sourceSerial = m_stereoSourceSerial;
            if (!m_stereoPair)
                m_stereoPair = std::make_unique<StereoPair>();
            m_stereoPair->job = job;
            if (result.layerActive && m_stereoPresenter)
                m_stereoPresenter->submit(std::move(job));
        }

        if (!resolved) {
            QMutexLocker lock(&m_frameMutex);
            result.layerActive = result.layerActive && bool(m_stereoPair);
            shownInLayer = result.layerActive;
        }
#else
        Q_UNUSED(plain);
#endif
        if (!shownInLayer && twoViews && (baseOnly || st.view != StereoFlat)) {
            // one view, in the scene graph: the left, if both are wanted and cannot be shown
            const bool fallback = multiview && st.view == StereoBothEyes;
            const int wanted = (baseOnly || fallback)
                    ? int(format.stereoFlags().testFlag(QVideoFrameFormat::StereoFlag::BaseViewRight))
                    : st.view == StereoRightEye ? 1 : 0;
            if (multiview) {
                // The ordinary fallback is the physical codec base view even
                // when the user corrects the order of the stereo presentation.
                const int index = (baseOnly || fallback) ? wanted : (rightFirst ? 1 - wanted : wanted);
                shown = qVideoFrameView(frame, index);
                displaySize = shown.size();
                result.eyeRect = QRectF(0, 0, 1, 1);
            } else if (packing.mode == StereoMode::FrameSequence) {
                skip = !repeated && eye != wanted;
                displaySize = frameSize;
            } else {
                const int index = rightFirst ? 1 - wanted : wanted;
                QRect rect;
                displaySize = viewSize(packing, frameSize);
                if (viewRect(packing, storedSize, index, &rect)) {
                    result.eyeRect = QRectF(qreal(rect.x()) / storedSize.width(), qreal(rect.y()) / storedSize.height(),
                                            qreal(rect.width()) / storedSize.width(), qreal(rect.height()) / storedSize.height());
                } else {
                    // the views are interleaved: taken apart on the CPU, into a frame of their own
                    const int depth = sourceDepth(format.pixelFormat());
                    const QImage image =
                            qImageFromVideoFrame(input,
                                                 depth > 12 ? QVideoFrameImageDepth::Float32
                                                            : QVideoFrameImageDepth::Float16);
                    QImage view = extractView(image, packing, index,
                                              st.scaling == StereoLanczos ? Scaling::Lanczos
                                                      : st.scaling == StereoBicubic ? Scaling::Bicubic : Scaling::Bilinear);
                    if (!view.isNull()) {
                        shown = QVideoFrame(view.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
                        shown.setStartTime(input.startTime());
                        shown.setEndTime(input.endTime());
                        displaySize = shown.size();
                    }
                }
            }
        }
        if (depthInput != StereoNone) {
            // a picture with its depth map is a 3D video in the depth layout
            result.video = true;
            result.packing = depthInput;
            if (!depthPictureRect.isNull()) {
                result.eyeRect = depthPictureRect;
                displaySize = QSizeF(frameSize.width() * depthPictureRect.width(),
                                     frameSize.height() * depthPictureRect.height())
                                      .toSize();
            }
        }
    } else {
        result.video = false;
        result.packing = StereoNone;
        result.sequence = 0;
    }

    // Shown in the compositor's stereo layer, the views are not taken by the scene graph: the layer's presenter draws them into a window surface of the
    // GPU (no read back), or maps the frame (a read back to system memory when the views are textures), converts it to the buffer format it hands
    // the compositor and copies it into shared memory. The route says which, and why.
#ifdef QT_MM_STEREO_LAYER
    if (!depthRoute.isEmpty() && result.layerActive) {
        QQuickVideoStereoPresenter::LayerRoute layerRoute;
        {
            QMutexLocker lock(&m_frameMutex);
            if (m_stereoPresenter)
                layerRoute = m_stereoPresenter->layerRoute();
        }
        depthRoute.insert(QStringLiteral("layer"), true);
        depthRoute.insert(QStringLiteral("layerDmabuf"), layerRoute.dmabuf);
        if (layerRoute.gpuActive) {
            depthRoute.insert(QStringLiteral("views"), QStringLiteral("texture"));
            depthRoute.insert(QStringLiteral("viewsWhy"),
                              QStringLiteral("the stereo layer draws the views into a window surface of the GPU (no read back, no copy of ours)"));
        } else if (depthRoute.value(QStringLiteral("views")).toString() == u"texture") {
            depthRoute.insert(QStringLiteral("views"), QStringLiteral("readBack"));
            depthRoute.insert(QStringLiteral("viewsWhy"),
                              layerRoute.copiedFrames || !layerRoute.why.isEmpty()
                                      ? QStringLiteral("the stereo layer takes the views from system memory: %1").arg(layerRoute.why)
                                      : QStringLiteral("the stereo layer takes the views from system memory (no frame has reached it yet)"));
        }
    }
#endif
    setDepthRoute(depthRoute);

    bool videoChanged = false;
    bool layerChanged = false;
    bool eyeChanged = false;
    {
        QMutexLocker lock(&m_frameMutex);
        videoChanged = m_stereo.video != result.video || m_stereo.packing != result.packing;
        layerChanged = m_stereo.layerActive != result.layerActive;
        eyeChanged = m_stereo.eyeRect != result.eyeRect;
        m_stereo.video = result.video;
        m_stereo.packing = result.packing;
        m_stereo.layerWanted = result.layerWanted;
#ifdef QT_MM_STEREO_LAYER
        if (!twoViews || (baseOnly && st.input == StereoNone)) {
            if (m_stereoPresenter)
                m_stereoPresenter->clear();
            m_stereoPair.reset();
            m_stereoHeldFrame = {};
            m_stereoHeldEye = -1;
            ++m_stereoSourceSerial;
        } else if (!result.layerWanted && m_stereoPresenter) {
            m_stereoPresenter->hide();
        }
#endif
        m_stereo.layerActive = result.layerActive;
        m_stereo.eyeRect = result.eyeRect;
        // only the frames of the video count, not a frame shown again
        if (!repeated)
            m_stereo.sequence = result.sequence;
        m_stereo.lastView = result.lastView;
        m_stereo.viewVaries = result.viewVaries;
        // the frame is kept, to be shown again when the choices change, only for 3D video
        if (twoViews || depthInput != StereoNone)
            m_stereoLastFrame = frame;
        else
            m_stereoLastFrame = QVideoFrame();
        if (skip)
            return;

        m_videoFormat = shown.surfaceFormat();
        m_frame = shown;
        m_frameDisplayingRotation = qNormalizedFrameTransformation(input, m_orientation).rotation;
        m_frameChanged = true;
    }

    if (videoChanged || layerChanged || result.layerWanted != st.layerWanted) {
        QMetaObject::invokeMethod(this, [this, videoChanged, layerChanged] {
            if (videoChanged)
                emit stereoVideoChanged();
            if (layerChanged)
                emit stereoLayerActiveChanged();
#ifdef QT_MM_STEREO_LAYER
            // the layer is made when a window shows 3D video, and let go of when it does not
            _q_syncStereoLayer();
#endif
        });
    }
    QMetaObject::invokeMethod(this, [this, displaySize, eyeChanged] {
        // another view of the frame, of the same size (the other eye of a still frame): the area
        // of the frame that is shown is set again
        if (eyeChanged)
            m_geometryDirty = true;
        _q_newFrame(displaySize);
    });
}

std::optional<std::chrono::nanoseconds> QQuickVideoOutput::g_signalBackoff;
void QQuickVideoOutput::setSignalBackoff(std::optional<std::chrono::nanoseconds> ns)
{
    g_signalBackoff = ns;
}

QT_END_NAMESPACE

#include "moc_qquickvideooutput_p.cpp"
