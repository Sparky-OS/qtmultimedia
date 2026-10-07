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
#include <QtCore/qtimer.h>
#include <QtQuick/qquickitem.h>

#include <QtMultimedia/qvideoframe.h>
#include <QtMultimedia/qvideoframeformat.h>
#include <QtMultimedia/qvideosink.h>
#include <QtMultimediaQuick/private/qtmultimediaquickglobal_p.h>

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
        StereoNone
    };
    Q_ENUM(StereoInput)

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
        StereoSurfaceBySource   // 8 bits for 8 bit video, 10 bits for 10 bit video, half float or 16 bits above
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

        // what the video in hand is
        bool video = false;                // two views in the frames
        StereoInput packing = StereoNone;  // as the stream or the input says (never Automatic)
        bool layerWanted = false;          // shown through the layer, if there is one
        bool layerActive = false;
        QRectF eyeRect = QRectF(0, 0, 1, 1); // the view in the frame, of the viewport, for the scene graph
        int sequence = 0;                  // frames of a frame sequence seen
        int lastView = -1;                 // the view the last frame of a sequence said it had
        bool viewVaries = false;           // the views of the frames of a sequence alternate: they say which eye
    };
    StereoState m_stereo;
    QVideoFrame m_stereoLastFrame;          // to show again when the choices change
    // The layer of the views, where the platform has one (QT_MM_STEREO_LAYER): kept in the class
    // whatever the build, so that every user of this header sees the same class
    QVideoFrame m_stereoHeldFrame;          // the first frame of a pair of a frame sequence
    int m_stereoHeldEye = -1;
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
