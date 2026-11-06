// Copyright (C) 2021 The Qt Company Ltd.
// Copyright (C) 2016 Research In Motion
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QQUICKVIDEOOUTPUT_P_H
#define QQUICKVIDEOOUTPUT_P_H

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
#include <QtCore/qpointer.h>
#include <QtCore/qmutex.h>
#include <QtCore/qvariant.h>
#include <QtCore/qtimer.h>
#include <QtQuick/qquickitem.h>

#include <QtMultimedia/qvideoframe.h>
#include <QtMultimedia/qvideoframeformat.h>
#include <QtMultimedia/qvideosink.h>
#include <QtMultimediaQuick/private/qtmultimediaquickglobal_p.h>
#include <QtMultimediaQuick/private/qquickvideodepth_p.h>

#include <atomic>
#include <memory>
#include <thread>

QT_BEGIN_NAMESPACE

class QQuickVideoStereoPresenter;

class QQuickVideoBackend;
class QVideoOutputOrientationHandler;
class QVideoSink;
class QSGVideoNode;
class QVideoFrameTexturePool;
using QVideoFrameTexturePoolWPtr = std::weak_ptr<QVideoFrameTexturePool>;

class QQuickVideoSink : public QVideoSink
{
    Q_OBJECT
    QML_NAMED_ELEMENT(VideoSink)
public:
    QQuickVideoSink(QObject *parent = nullptr) : QVideoSink(parent)
    {
        connect(this, &QVideoSink::videoFrameChanged, this, &QQuickVideoSink::videoFrameChanged,
                Qt::DirectConnection);
    }

Q_SIGNALS:
    void videoFrameChanged();
};

class Q_MULTIMEDIAQUICK_EXPORT QQuickVideoOutput : public QQuickItem
{
    Q_OBJECT
    Q_DISABLE_COPY(QQuickVideoOutput)
    Q_PROPERTY(FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(EndOfStreamPolicy endOfStreamPolicy READ endOfStreamPolicy WRITE setEndOfStreamPolicy
                       NOTIFY endOfStreamPolicyChanged REVISION(6, 9))
    Q_PROPERTY(int orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)
    Q_PROPERTY(bool mirrored READ mirrored WRITE setMirrored NOTIFY mirroredChanged REVISION(6, 9))
    Q_PROPERTY(QRectF sourceRect READ sourceRect NOTIFY sourceRectChanged)
    Q_PROPERTY(QRectF contentRect READ contentRect NOTIFY contentRectChanged)
    Q_PROPERTY(QVideoSink* videoSink READ videoSink CONSTANT)
    Q_PROPERTY(StereoInput stereoInput READ stereoInput WRITE setStereoInput NOTIFY stereoInputChanged)
    Q_PROPERTY(bool stereoSwapEyes READ stereoSwapEyes WRITE setStereoSwapEyes NOTIFY stereoSwapEyesChanged)
    Q_PROPERTY(StereoView stereoView READ stereoView WRITE setStereoView NOTIFY stereoViewChanged)
    Q_PROPERTY(bool stereoShapeFallback READ stereoShapeFallback WRITE setStereoShapeFallback
                       NOTIFY stereoShapeFallbackChanged)
    Q_PROPERTY(StereoScaling stereoScaling READ stereoScaling WRITE setStereoScaling
                       NOTIFY stereoScalingChanged)
    Q_PROPERTY(StereoSurface stereoSurface READ stereoSurface WRITE setStereoSurface
                       NOTIFY stereoSurfaceChanged)
    Q_PROPERTY(bool stereoVideo READ stereoVideo NOTIFY stereoVideoChanged)
    Q_PROPERTY(StereoInput stereoPacking READ stereoPacking NOTIFY stereoVideoChanged)
    Q_PROPERTY(bool stereoLayerSupported READ stereoLayerSupported CONSTANT)
    Q_PROPERTY(bool stereoLayerActive READ stereoLayerActive NOTIFY stereoLayerActiveChanged)
    Q_PROPERTY(bool stereoDepth READ stereoDepth WRITE setStereoDepth NOTIFY stereoDepthChanged)
    Q_PROPERTY(StereoDepthView stereoDepthView READ stereoDepthView WRITE setStereoDepthView
                       NOTIFY stereoDepthViewChanged)
    Q_PROPERTY(StereoDepthWarp stereoDepthWarp READ stereoDepthWarp WRITE setStereoDepthWarp
                       NOTIFY stereoDepthWarpChanged)
    Q_PROPERTY(StereoDepthFill stereoDepthFill READ stereoDepthFill WRITE setStereoDepthFill
                       NOTIFY stereoDepthFillChanged)
    Q_PROPERTY(StereoDepthSmooth stereoDepthSmooth READ stereoDepthSmooth WRITE setStereoDepthSmooth
                       NOTIFY stereoDepthSmoothChanged)
    Q_PROPERTY(qreal stereoDepthSigmaH READ stereoDepthSigmaH WRITE setStereoDepthSigmaH
                       NOTIFY stereoDepthSigmaHChanged)
    Q_PROPERTY(qreal stereoDepthSigmaV READ stereoDepthSigmaV WRITE setStereoDepthSigmaV
                       NOTIFY stereoDepthSigmaVChanged)
    Q_PROPERTY(int stereoDepthRadius READ stereoDepthRadius WRITE setStereoDepthRadius
                       NOTIFY stereoDepthRadiusChanged)
    Q_PROPERTY(qreal stereoDepthRange READ stereoDepthRange WRITE setStereoDepthRange
                       NOTIFY stereoDepthRangeChanged)
    Q_PROPERTY(qreal stereoDepthZero READ stereoDepthZero WRITE setStereoDepthZero
                       NOTIFY stereoDepthZeroChanged)
    Q_PROPERTY(qreal stereoDepthShift READ stereoDepthShift WRITE setStereoDepthShift
                       NOTIFY stereoDepthShiftChanged)
    Q_PROPERTY(StereoDepthMeaning stereoDepthMeaning READ stereoDepthMeaning
                       WRITE setStereoDepthMeaning NOTIFY stereoDepthMeaningChanged)
    Q_PROPERTY(qreal stereoDepthNear READ stereoDepthNear WRITE setStereoDepthNear
                       NOTIFY stereoDepthNearChanged)
    Q_PROPERTY(qreal stereoDepthFar READ stereoDepthFar WRITE setStereoDepthFar
                       NOTIFY stereoDepthFarChanged)
    Q_PROPERTY(QString stereoDepthMark READ stereoDepthMark WRITE setStereoDepthMark
                       NOTIFY stereoDepthMarkChanged)
    Q_PROPERTY(bool stereoDepthAutomatic READ stereoDepthAutomatic WRITE setStereoDepthAutomatic
                       NOTIFY stereoDepthAutomaticChanged)
    Q_PROPERTY(bool stereoDepthGpu READ stereoDepthGpu WRITE setStereoDepthGpu
                       NOTIFY stereoDepthGpuChanged)
    Q_PROPERTY(QVariantMap stereoDepthChoice READ stereoDepthChoice NOTIFY stereoDepthChoiceChanged)
    Q_PROPERTY(QVariantMap stereoDepthGpuMemory READ stereoDepthGpuMemory NOTIFY stereoDepthGpuMemoryChanged)
    Q_PROPERTY(QVariantMap stereoDepthRoute READ stereoDepthRoute NOTIFY stereoDepthRouteChanged)
    Q_MOC_INCLUDE(qvideosink.h)
    Q_MOC_INCLUDE(qvideoframe.h)
    QML_NAMED_ELEMENT(VideoOutput)

public:

    enum FillMode
    {
        Stretch            = Qt::IgnoreAspectRatio,
        PreserveAspectFit  = Qt::KeepAspectRatio,
        PreserveAspectCrop = Qt::KeepAspectRatioByExpanding
    };
    Q_ENUM(FillMode)

    enum EndOfStreamPolicy
    {
        ClearOutput,
        KeepLastFrame
    };
    Q_ENUM(EndOfStreamPolicy)

    // How the frames of a stereoscopic 3D video hold the two views: the packings of H.264
    // Table D-8 and H.265 Table D.8, which Matroska's StereoMode (RFC 9559) shares. StereoAutomatic
    // takes what the stream says (QVideoFrameFormat::stereoMode()), the others say it for a
    // stream that does not. Side by side and top and bottom are "half" when each view is
    // squeezed to half the frame and "full" when each has the frame's size; without the word the
    // shape of the frame decides. The eye order comes with the stream, stereoSwapEyes turns it round
    enum StereoInput
    {
        StereoAutomatic,
        StereoSideBySide,
        StereoSideBySideHalf,
        StereoSideBySideFull,
        StereoTopBottom,
        StereoTopBottomHalf,
        StereoTopBottomFull,
        StereoSideBySideQuincunx,
        StereoRowInterleaved,
        StereoColumnInterleaved,
        StereoCheckerboard,
        StereoFrameSequence,
        StereoAnaglyphCyanRed,
        StereoAnaglyphGreenMagenta,
        StereoNone,
        // A picture with its depth map (2D plus depth), from which the views are drawn: the depth
        // layouts of Sparky Stereo OS's depth marks (QVideoFrameFormat::StereoMode)
        StereoDepthSideBySide,
        StereoDepthSideBySideDepthFirst,
        StereoDepthTopBottom,
        StereoDepthTopBottomDepthFirst,
        StereoDepthGraphics,
        StereoDepthBackground,
        StereoDepthSeparate
    };
    Q_ENUM(StereoInput)

    // The views drawn from a depth map: the choices of libstereo-dibr, see the properties
    enum StereoDepthView
    {
        StereoDepthPictureLeft,
        StereoDepthPictureCenter
    };
    Q_ENUM(StereoDepthView)
    enum StereoDepthWarp
    {
        StereoDepthForward,
        StereoDepthBackward,
        StereoDepthLayered
    };
    Q_ENUM(StereoDepthWarp)
    enum StereoDepthFill
    {
        StereoDepthFillBackground,
        StereoDepthFillDepth,
        StereoDepthFillTelea,
        StereoDepthFillNone,
        StereoDepthFillPyramid
    };
    Q_ENUM(StereoDepthFill)
    enum StereoDepthSmooth
    {
        StereoDepthSmoothOff,
        StereoDepthSmoothGaussian
    };
    Q_ENUM(StereoDepthSmooth)
    enum StereoDepthMeaning
    {
        StereoDepthAutomatic = -1,
        StereoDepthNearness,
        StereoDepthFarness,
        StereoDepthInverseZ,
        StereoDepthDisparity,
        StereoDepthZ,
        StereoDepthNonlinearDisparity,
        StereoDepthParallax,
        StereoDepthRangeLinear,
        StereoDepthRangeInverse,
        StereoDepthMetric
    };
    Q_ENUM(StereoDepthMeaning)

    // What is shown of a stereoscopic 3D video
    enum StereoView
    {
        StereoBothEyes,  // each eye its view: on a stereo compositor, in a surface of its own
        StereoLeftEye,
        StereoRightEye,
        StereoFlat       // the frame as it is stored, as if it were not 3D
    };
    Q_ENUM(StereoView)

    // The filter that restores the size of a view, or of the interleaved views
    enum StereoScaling
    {
        StereoBilinear,
        StereoBicubic,
        StereoLanczos
    };
    Q_ENUM(StereoScaling)

    // The depth of the surface the views are shown in
    enum StereoSurface
    {
        StereoSurface8Bit,
        StereoSurface10Bit,
        StereoSurfaceHalfFloat, // or 16 bits, where the compositor has no half float
        StereoSurface16Bit,     // 16 bits per channel, unsigned normalised: the depth of 12 and 16 bit video
        StereoSurfaceBySource   // 8 bits for 8 bit video, 10 bits for 10 bit video, 16 bits above
    };
    Q_ENUM(StereoSurface)

    QQuickVideoOutput(QQuickItem *parent = 0);
    ~QQuickVideoOutput() override;

    Q_INVOKABLE QVideoSink *videoSink() const;

    FillMode fillMode() const;
    void setFillMode(FillMode mode);

    int orientation() const;
    void setOrientation(int);

    bool mirrored() const;
    void setMirrored(bool);

    QRectF sourceRect() const;
    QRectF contentRect() const;

    EndOfStreamPolicy endOfStreamPolicy() const;
    void setEndOfStreamPolicy(EndOfStreamPolicy policy);

    Q_REVISION(6, 9) Q_INVOKABLE void clearOutput();

    StereoInput stereoInput() const;
    void setStereoInput(StereoInput input);
    bool stereoSwapEyes() const;
    void setStereoSwapEyes(bool swap);
    StereoView stereoView() const;
    void setStereoView(StereoView view);
    bool stereoShapeFallback() const;
    void setStereoShapeFallback(bool fallback);
    StereoScaling stereoScaling() const;
    void setStereoScaling(StereoScaling scaling);
    StereoSurface stereoSurface() const;
    void setStereoSurface(StereoSurface surface);
    bool stereoVideo() const;
    StereoInput stereoPacking() const;
    static bool stereoLayerSupported();
    bool stereoLayerActive() const;
    // the surface the views are drawn into for a video of this depth in bits (the person's choice, or the depth of the video for StereoSurfaceBySource)
    static StereoSurface stereoSurfaceFor(StereoSurface chosen, int sourceDepth);
    // how many times the source of the views changed (the presenter is cleared each time)
    quint64 stereoSourceSerial() const;
    bool stereoDepth() const;
    void setStereoDepth(bool value);
    StereoDepthView stereoDepthView() const;
    void setStereoDepthView(StereoDepthView value);
    StereoDepthWarp stereoDepthWarp() const;
    void setStereoDepthWarp(StereoDepthWarp value);
    StereoDepthFill stereoDepthFill() const;
    void setStereoDepthFill(StereoDepthFill value);
    StereoDepthSmooth stereoDepthSmooth() const;
    void setStereoDepthSmooth(StereoDepthSmooth value);
    qreal stereoDepthSigmaH() const;
    void setStereoDepthSigmaH(qreal value);
    qreal stereoDepthSigmaV() const;
    void setStereoDepthSigmaV(qreal value);
    int stereoDepthRadius() const;
    void setStereoDepthRadius(int value);
    qreal stereoDepthRange() const;
    void setStereoDepthRange(qreal value);
    qreal stereoDepthZero() const;
    void setStereoDepthZero(qreal value);
    qreal stereoDepthShift() const;
    void setStereoDepthShift(qreal value);
    StereoDepthMeaning stereoDepthMeaning() const;
    void setStereoDepthMeaning(StereoDepthMeaning value);
    qreal stereoDepthNear() const;
    void setStereoDepthNear(qreal value);
    qreal stereoDepthFar() const;
    void setStereoDepthFar(qreal value);
    QString stereoDepthMark() const;
    bool stereoDepthAutomatic() const;
    void setStereoDepthAutomatic(bool value);
    bool stereoDepthGpu() const;
    void setStereoDepthGpu(bool value);
    QVariantMap stereoDepthChoice() const;
    QVariantMap stereoDepthGpuMemory() const;
    QVariantMap stereoDepthRoute() const;
    // The frames read back to system memory so far: the decoder's (\c frames) and the drawn views' (\c views)
    Q_INVOKABLE QVariantMap stereoDepthReadBacks() const;
    void setStereoDepthMark(const QString &value);

Q_SIGNALS:
    void sourceChanged();
    void fillModeChanged(QQuickVideoOutput::FillMode);
    void orientationChanged();
    void mirroredChanged();
    void sourceRectChanged();
    void contentRectChanged();
    void endOfStreamPolicyChanged(QQuickVideoOutput::EndOfStreamPolicy);
    void stereoInputChanged();
    void stereoSwapEyesChanged();
    void stereoViewChanged();
    void stereoShapeFallbackChanged();
    void stereoScalingChanged();
    void stereoSurfaceChanged();
    void stereoVideoChanged();
    void stereoLayerActiveChanged();
    void stereoDepthChanged();
    void stereoDepthViewChanged();
    void stereoDepthWarpChanged();
    void stereoDepthFillChanged();
    void stereoDepthSmoothChanged();
    void stereoDepthSigmaHChanged();
    void stereoDepthSigmaVChanged();
    void stereoDepthRadiusChanged();
    void stereoDepthRangeChanged();
    void stereoDepthZeroChanged();
    void stereoDepthShiftChanged();
    void stereoDepthMeaningChanged();
    void stereoDepthNearChanged();
    void stereoDepthFarChanged();
    void stereoDepthMarkChanged();
    void stereoDepthAutomaticChanged();
    void stereoDepthGpuChanged();
    void stereoDepthChoiceChanged();
    void stereoDepthGpuMemoryChanged();
    void stereoDepthRouteChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &changeData) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void releaseResources() override;

private:
    QSize nativeSize() const;
    void updateGeometry();
    QRectF adjustedViewport() const;

    void setFrame(const QVideoFrame &frame, bool repeated = false);
    void processStereo(QVideoFrame *frame, QSize *displaySize, bool repeated);
    void ensureStereoPresenter();
    void reprocessFrame();

    void initRhiForSink();
    void updateHdr(QSGVideoNode *videoNode);
    void disconnectWindowConnections();

private Q_SLOTS:
    void _q_newFrame(QSize);
    void _q_updateGeometry();
    void _q_syncStereoLayer();

private:
    QSize m_nativeSize;

    bool m_geometryDirty = true;
    QRectF m_lastRect;      // Cache of last rect to avoid recalculating geometry
    QRectF m_contentRect;   // Destination pixel coordinates, unclipped
    int m_orientation = 0;
    bool m_mirrored = false;
    QtVideo::Rotation m_frameDisplayingRotation = QtVideo::Rotation::None;
    Qt::AspectRatioMode m_aspectRatioMode = Qt::KeepAspectRatio;

    QPointer<QQuickWindow> m_window;
    QVideoSink *m_sink = nullptr;
    QVideoFrameFormat m_videoFormat;

    QVideoFrameTexturePoolWPtr m_texturePool;
    QVideoFrame m_frame;
    bool m_frameChanged = false;
    mutable QMutex m_frameMutex;
    QRectF m_renderedRect;         // Destination pixel coordinates, clipped
    QRectF m_sourceTextureRect;    // Source texture coordinates

    EndOfStreamPolicy m_endOfStreamPolicy = ClearOutput;

    // Stereoscopic 3D, see the properties. All of it is guarded by m_frameMutex
    struct StereoState
    {
        StereoInput input = StereoAutomatic;
        bool swapEyes = false;
        StereoView view = StereoBothEyes;
        bool shapeFallback = true;
        StereoScaling scaling = StereoLanczos;
        StereoSurface surface = StereoSurfaceBySource;
        // the views drawn from a depth map
        QQuickVideoDepth::Options depth;

        // what the video in hand is
        bool video = false;                // two views in the frames
        StereoInput packing = StereoNone;  // as the stream or the input says (never Automatic)
        // the packing of the frame as it is shown (a depth video: the full side by side of its drawn views, where packing says the depth layout)
        StereoInput shownPacking = StereoNone;
        bool layerWanted = false;          // shown through the layer, if there is one
        bool layerActive = false;
        bool cropGeometrySupported = false;
        QRectF eyeRect = QRectF(0, 0, 1, 1); // the view in the frame, of the viewport, for the scene graph
        int sequence = 0;                  // frames of a frame sequence seen
        int lastView = -1;                 // the view the last frame of a sequence said it had
        bool viewVaries = false;           // the views of the frames of a sequence alternate: they say which eye
    };
    StereoState m_stereo;
    QVideoFrame m_stereoLastFrame;          // to show again when the choices change
    QVideoFrameFormat m_stereoLastShownFormat; // the format of the frame as it was shown (a depth video: of the drawn views)
    // draws the views of a picture with its depth map (libstereo-dibr), from the threads of the
    // video and of the item in turn
    mutable QMutex m_depthMutex;
    std::unique_ptr<QQuickVideoDepth::Renderer> m_depthRenderer;
    // what the automatic way chose for the video that plays (guarded by m_frameMutex)
    QVariantMap m_depthChoice;
    void setDepthChoice(const QQuickVideoDepth::Choice &choice);
    QVariantMap m_depthGpuMemory;
    void setDepthGpuMemory(const QQuickVideoDepth::GpuMemory &memory);
    QVariantMap m_depthRoute;
    void setDepthRoute(const QVariantMap &route);
    // The layer of the views, where the platform has one (QT_MM_STEREO_LAYER): kept in the class
    // whatever the build, so that every user of this header sees the same class
    struct StereoPair;
    std::unique_ptr<StereoPair> m_stereoPair; // survives temporary layer/window replacement
    quint64 m_stereoSourceSerial = 0;
    QVideoFrame m_stereoHeldFrame;          // the first frame of a pair of a frame sequence
    int m_stereoHeldEye = -1;               // physical eye, independent of input eye swap
    std::shared_ptr<QQuickVideoStereoPresenter> m_stereoPresenter;
    std::atomic_bool m_stereoPresenterReady = false;
    QTimer m_stereoTimer;
    QRect m_stereoRect;

    struct DestructorGuard
    {
        QMutex m_mutex;
        bool m_isAlive{ true };

        template <typename Functor>
        void runWhileAlive(Functor &&f)
        {
            QMutexLocker lock(&m_mutex);
            if (m_isAlive)
                f();
        }
    };

    std::shared_ptr<DestructorGuard> m_destructorGuard = std::make_shared<DestructorGuard>();

    template <typename Functor>
    auto makeGuardedCall(Functor &&f)
    {
        return [f = std::forward<Functor>(f), guard = m_destructorGuard](auto... params) {
            if (g_signalBackoff) {
                Q_UNLIKELY_BRANCH;
                std::this_thread::sleep_for(*g_signalBackoff);
            }

            guard->runWhileAlive([&] {
                f(params...);
            });
        };
    }

    // for testing
    static std::optional<std::chrono::nanoseconds> g_signalBackoff;

public:
    static void setSignalBackoff(std::optional<std::chrono::nanoseconds>);
};

QT_END_NAMESPACE

#endif // QQUICKVIDEOOUTPUT_P_H
