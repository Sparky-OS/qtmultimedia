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
        bool swapEyes = false;
        QQuickVideoStereo::Scaling scaling = QQuickVideoStereo::Scaling::Bilinear;
        QQuickVideoStereo::BufferFormat format = QQuickVideoStereo::BufferFormat::Xrgb8888;
        // the eye (0 left, 1 right) a frame of a FrameSequence is for
        int frameView = 0;
        // 16 bit video is converted to 32 bit floating point, the rest to 16 bit
        bool wide = false;
    };

    explicit QQuickVideoStereoPresenter(QWindow *window);
    ~QQuickVideoStereoPresenter();

    // False if the layer could not be made (no stereo compositor, window not created)
    bool isValid() const { return m_valid; }
    void setRect(const QRect &rect);
    void submit(Job job);
    void hide();

private:
    // The thread makes the layer, shows into it, and lets it go: the library that declares the
    // stereo content keeps its state per thread
    void run() override;
    void process(Job &job);

    QWindow *m_window;
    std::unique_ptr<QQuickVideoStereoLayer> m_layer;
    QMutex m_mutex;
    QWaitCondition m_condition;
    Job m_pending;
    bool m_hasPending = false;
    bool m_stopping = false;
    bool m_created = false;
    bool m_valid = false;
};

QT_END_NAMESPACE

#endif
