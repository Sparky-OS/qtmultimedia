// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QQUICKVIDEOSTEREOPRESENTER_P_H
#define QQUICKVIDEOSTEREOPRESENTER_P_H

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

#include <QtCore/qmutex.h>
#include <QtCore/qthread.h>
#include <QtCore/qwaitcondition.h>
#include <QtMultimedia/qvideoframe.h>
#include <QtMultimediaQuick/private/qquickvideostereo_p.h>
#include <QtMultimediaQuick/private/qquickvideostereolayer_p.h>

#include <atomic>
#include <memory>

QT_BEGIN_NAMESPACE

class QWindow;

// Turns the frames of a stereoscopic 3D video into the pair of views the layer shows, off the
// threads of the video and of the scene graph, keeping only the newest frame when it is slower
// than the video
class QQuickVideoStereoPresenter : private QThread
{
public:
    struct Job
    {
        QVideoFrame frame;
        // the other frame of the pair of a FrameSequence, for the other eye
        QVideoFrame otherFrame;
        QQuickVideoStereo::Packing packing;
        QSize displaySize;
        quint64 sourceSerial = 0;
        bool swapEyes = false;
        QQuickVideoStereo::Scaling scaling = QQuickVideoStereo::Scaling::Bilinear;
        QQuickVideoStereo::BufferFormat format = QQuickVideoStereo::BufferFormat::Xrgb8888;
        // the eye (0 left, 1 right) a frame of a FrameSequence is for
        int frameView = 0;
        // 16 bit video is converted to 32 bit floating point, the rest to 16 bit
        bool wide = false;
    };

    struct Geometry
    {
        QRect destination;
        bool crop = false;
        bool operator==(const Geometry &other) const
        { return destination == other.destination && crop == other.crop; }
    };

    // Protected by the presenter's mutex. Source, input interpretation, placement
    // and visibility invalidate an in-flight conversion. Newer frames replace pending
    // work without starving the single converter's still-valid publication.
    struct State
    {
        struct Work { Job job; Geometry geometry; quint64 revision; bool visible; };
        Geometry geometry;
        Job current, pending;
        quint64 revision = 0;
        quint64 sourceSerial = 0;
        bool visible = false;
        bool dirty = false;
        bool retry = false;

        void setGeometry(Geometry value)
        {
            if (visible && geometry == value)
                return;
            retry = false;
            geometry = value;
            visible = !value.destination.isEmpty();
            ++revision;
            dirty = true;
        }
        void submit(Job value)
        {
            if (!value.frame.isValid())
                return;
            if (value.packing.mode == QVideoFrameFormat::StereoMode::FrameSequence
                    && !value.frame.surfaceFormat().stereoFlags().testFlag(QVideoFrameFormat::StereoFlag::Multiview)
                    && !value.otherFrame.isValid())
                return;
            const Job &previous = pending.frame.isValid() ? pending : current;
            const bool interpretationChanged = previous.frame.isValid()
                    && (previous.packing.mode != value.packing.mode
                        || previous.packing.size != value.packing.size
                        || previous.packing.rightEyeFirst != value.packing.rightEyeFirst
                        || previous.swapEyes != value.swapEyes
                        || previous.displaySize != value.displaySize
                        || previous.format != value.format || previous.wide != value.wide);
            if (value.sourceSerial != sourceSerial || interpretationChanged) {
                current = {};
                ++revision;
            }
            sourceSerial = value.sourceSerial;
            pending = std::move(value);
            retry = false;
            dirty = true;
        }
        void hide()
        {
            retry = false;
            visible = false;
            ++revision;
            dirty = true;
        }
        void clear()
        {
            current = {};
            pending = {};
            hide();
        }
        Work take()
        {
            if (pending.frame.isValid()) {
                current = std::move(pending);
                pending = {};
            }
            dirty = false;
            return { current, geometry, revision, visible && current.frame.isValid() };
        }
        bool isCurrent(quint64 value) const { return revision == value; }
        void retryPresentation(quint64 value)
        {
            if (isCurrent(value) && visible && current.frame.isValid()) {
                dirty = true;
                retry = true;
            }
        }
    };

    explicit QQuickVideoStereoPresenter(QWindow *window);
    ~QQuickVideoStereoPresenter();

    // False if the layer could not be made (no stereo compositor, window not created)
    bool isValid() const { return m_valid; }
    void setGeometry(Geometry geometry);
    void clear();
    void submit(Job job);
    void hide();

private:
    // The thread makes the layer, shows into it, and lets it go: the library that declares the
    // stereo content keeps its state per thread
    void run() override;
    void process(State::Work work);

    QWindow *m_window;
    std::unique_ptr<QQuickVideoStereoLayer> m_layer;
    QMutex m_mutex;
    QWaitCondition m_condition;
    State m_state;
    bool m_stopping = false;
    bool m_created = false;
    bool m_valid = false;
};

QT_END_NAMESPACE

#endif
