// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#ifndef QFFMPEGSTREAMDECODER_P_H
#define QFFMPEGSTREAMDECODER_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API. It exists purely as an
// implementation detail. This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//
#include <QtFFmpegMediaPluginImpl/private/qffmpegplaybackengineobject_p.h>
#include <QtFFmpegMediaPluginImpl/private/qffmpegframe_p.h>
#include <QtFFmpegMediaPluginImpl/private/qffmpegpacket_p.h>
#include <QtFFmpegMediaPluginImpl/private/qffmpegplaybackutils_p.h>
#include <QtMultimedia/private/qplatformmediaplayer_p.h>

#include <QtCore/qqueue.h>

#include <deque>
#include <optional>

QT_BEGIN_NAMESPACE

namespace QFFmpeg {

class StreamDecoder : public PlaybackEngineObject
{
    Q_OBJECT
public:
    // depthCodecContext: the decoder of the stream that holds the depth maps of the pictures (an
    // ISO/IEC 23002-3 auxiliary video stream, an MP4 auxiliary depth track), whose packets come
    // to this decoder too
    StreamDecoder(const PlaybackEngineObjectID &id, const CodecContext &codecContext,
                  TrackPosition absSeekPos,
                  const std::optional<CodecContext> &depthCodecContext = std::nullopt);

    ~StreamDecoder() override;

    void seek(quint64 sessionID, TrackPosition pos, const LoopOffset &offset);

    QPlatformMediaPlayer::TrackType trackType() const;

    // Maximum number of frames that we are allowed to keep in render queue
    static qint32 maxQueueSize(QPlatformMediaPlayer::TrackType type);

public slots:

    void decode(Packet);

    void onFinalPacketReceived(PlaybackEngineObjectID sourceID);

    void onFrameProcessed(const Frame &frame);

signals:
    void requestHandleFrame(Frame frame);

    void packetProcessed(Packet);

protected:
    bool canDoNextStep() const override;

    void doNextStep() override;

private:
    void decodeMedia(const Packet &packet);

    void decodeSubtitle(const Packet &packet);

    void onFrameFound(const Frame &frame);

    int sendAVPacket(const Packet &packet);

    void receiveAVFrames(bool flushPacket = false);

    // Depth as a 3D source: a picture is handed on packed with its depth map (the depth auxiliary
    // layer of an H.265 stream, or a depth stream of its own) side by side, in one frame
    // (QFFmpeg::packPictureAndDepth). A picture waits for its depth map while one is to come.
    struct DepthPending
    {
        AVFrameUPtr frame;
        qint64 time = 0; // AV_TIME_BASE units
    };
    bool depthExpected() const;
    void decodeDepth(const Packet &packet);
    void receiveDepthFrames();
    void holdPicture(AVFrameUPtr frame);
    void holdDepth(AVFrameUPtr frame, const AVStream *stream);
    void pairDepth(bool final);
    void clearDepth();

private:
    CodecContext m_codecContext;
    std::optional<CodecContext> m_depthCodecContext;
    std::deque<DepthPending> m_heldPictures;
    std::deque<DepthPending> m_heldDepths;
    bool m_depthLayer = false; // a depth picture of the stream's depth layer came

    const QPlatformMediaPlayer::TrackType m_trackType;

    struct SessionContext
    {
        TrackPosition absSeekPos = TrackPosition(0);
        qint32 pendingFramesCount = 0;
        LoopOffset offset = {};
        QQueue<Packet> packets = {};
    };

    SessionContext m_sessionCtx;
};

} // namespace QFFmpeg

QT_END_NAMESPACE

#endif // QFFMPEGSTREAMDECODER_P_H
