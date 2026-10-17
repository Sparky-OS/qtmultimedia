// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "playbackengine/qffmpegstreamdecoder_p.h"
#include "playbackengine/qffmpegmediadataholder_p.h"
#include "qffmpegdepth_p.h"
#include "qffmpeghwaccel_p.h"
#include <qloggingcategory.h>

QT_BEGIN_NAMESPACE

Q_STATIC_LOGGING_CATEGORY(qLcStreamDecoder, "qt.multimedia.ffmpeg.streamdecoder");

namespace QFFmpeg {

StreamDecoder::StreamDecoder(const PlaybackEngineObjectID &id, const CodecContext &codecContext,
                             TrackPosition absSeekPos,
                             const std::optional<CodecContext> &depthCodecContext)
    : PlaybackEngineObject(id),
      m_codecContext(codecContext),
      m_depthCodecContext(depthCodecContext),
      m_trackType(MediaDataHolder::trackTypeFromMediaType(codecContext.context()->codec_type)),
      m_sessionCtx{ absSeekPos }
{
    qCDebug(qLcStreamDecoder) << "Create stream decoder, trackType" << m_trackType
                              << "absSeekPos:" << absSeekPos.get() << "depth stream:"
                              << (m_depthCodecContext ? int(m_depthCodecContext->streamIndex())
                                                      : -1);
    Q_ASSERT(m_trackType != QPlatformMediaPlayer::NTrackTypes);
}

StreamDecoder::~StreamDecoder()
{
    avcodec_flush_buffers(m_codecContext.context());
    if (m_depthCodecContext)
        avcodec_flush_buffers(m_depthCodecContext->context());
}

void StreamDecoder::seek(quint64 sessionID, TrackPosition pos, const LoopOffset &offset)
{
    updateSession(sessionID, [this, pos, offset]() {
        m_sessionCtx = { offset.loopStartTimeUs.asDuration() + pos };
        avcodec_flush_buffers(m_codecContext.context());
        if (m_depthCodecContext)
            avcodec_flush_buffers(m_depthCodecContext->context());
        clearDepth();
    });
}

void StreamDecoder::onFinalPacketReceived(PlaybackEngineObjectID sourceID)
{
    if (checkSessionID(sourceID.sessionID))
        decode({});
}

void StreamDecoder::decode(Packet packet)
{
    if (packet.isValid() && !checkSessionID(packet.sourceID().sessionID)) {
        qCDebug(qLcStreamDecoder) << "Packet session outdated. Source id:" << packet.sourceID()
                                  << "current id" << id();
        // no need to report packetProcessed: demuxer must be cleaned up
        return;
    }

    m_sessionCtx.packets.enqueue(std::move(packet));
    scheduleNextStep();
}

void StreamDecoder::doNextStep()
{
    Packet packet = m_sessionCtx.packets.dequeue();

    auto decodePacket = [this](const Packet &packet) {
        if (trackType() == QPlatformMediaPlayer::SubtitleStream)
            decodeSubtitle(packet);
        else
            decodeMedia(packet);
    };

    if (packet.isValid() && packet.loopOffset().loopIndex != m_sessionCtx.offset.loopIndex) {
        decodePacket({});

        qCDebug(qLcStreamDecoder) << "flush buffers due to new loop:"
                                  << packet.loopOffset().loopIndex;

        avcodec_flush_buffers(m_codecContext.context());
        if (m_depthCodecContext)
            avcodec_flush_buffers(m_depthCodecContext->context());
        clearDepth();
        m_sessionCtx.offset = packet.loopOffset();
    }

    decodePacket(packet);

    setAtEnd(!packet.isValid());

    if (packet.isValid())
        emit packetProcessed(std::move(packet));

    scheduleNextStep();
}

QPlatformMediaPlayer::TrackType StreamDecoder::trackType() const
{
    return m_trackType;
}

qint32 StreamDecoder::maxQueueSize(QPlatformMediaPlayer::TrackType type)
{
    switch (type) {

    case QPlatformMediaPlayer::VideoStream:
        return 3;
    case QPlatformMediaPlayer::AudioStream:
        return 9;
    case QPlatformMediaPlayer::SubtitleStream:
        return 6; /*main packet and closing packet*/
    default:
        Q_UNREACHABLE_RETURN(-1);
    }
}

void StreamDecoder::onFrameProcessed(const Frame &frame)
{
    if (!checkID(frame.sourceID()))
        return;

    --m_sessionCtx.pendingFramesCount;
    Q_ASSERT(m_sessionCtx.pendingFramesCount >= 0);

    scheduleNextStep();
}

bool StreamDecoder::canDoNextStep() const
{
    const qint32 maxCount = maxQueueSize(m_trackType);

    return !m_sessionCtx.packets.empty() && m_sessionCtx.pendingFramesCount < maxCount
            && PlaybackEngineObject::canDoNextStep();
}

void StreamDecoder::onFrameFound(const Frame &frame)
{
    if (frame.isValid() && frame.absoluteEnd() < m_sessionCtx.absSeekPos)
        return;

    Q_ASSERT(m_sessionCtx.pendingFramesCount >= 0);
    ++m_sessionCtx.pendingFramesCount;
    emit requestHandleFrame(frame);
}

void StreamDecoder::decodeMedia(const Packet &packet)
{
    if (m_depthCodecContext && packet.isValid()
        && packet.avPacket()->stream_index == int(m_depthCodecContext->streamIndex())) {
        decodeDepth(packet);
        return;
    }
    if (m_depthCodecContext && !packet.isValid())
        decodeDepth(packet); // the end: the last depth maps first

    auto sendPacketResult = sendAVPacket(packet);

    if (sendPacketResult == AVERROR(EAGAIN)) {
        // Doc says:
        //  AVERROR(EAGAIN): input is not accepted in the current state - user
        //                   must read output with avcodec_receive_frame() (once
        //                   all output is read, the packet should be resent, and
        //                   the call will not fail with EAGAIN).
        receiveAVFrames();
        sendPacketResult = sendAVPacket(packet);

        if (sendPacketResult != AVERROR(EAGAIN))
            qWarning() << "Unexpected FFmpeg behavior";
    }

    if (sendPacketResult == 0)
        receiveAVFrames(!packet.isValid());
}

int StreamDecoder::sendAVPacket(const Packet &packet)
{
    return avcodec_send_packet(m_codecContext.context(), packet.isValid() ? packet.avPacket() : nullptr);
}

void StreamDecoder::receiveAVFrames(bool flushPacket)
{
    while (true) {
        auto avFrame = makeAVFrame();

        const auto receiveFrameResult = avcodec_receive_frame(m_codecContext.context(), avFrame.get());

        if (receiveFrameResult == AVERROR_EOF || receiveFrameResult == AVERROR(EAGAIN)) {
            if (flushPacket && receiveFrameResult == AVERROR(EAGAIN)) {
            // The documentation says that in the EAGAIN state output is not available. The new
            // input must be sent. It does not say that this state can also be returned for
            // Android MediaCodec when the ff_AMediaCodec_dequeueOutputBuffer call times out.
            // The flush packet means it is the end of the stream. No more packets are available,
            // so getting EAGAIN is unexpected here. At this point, the EAGAIN status was probably
            // caused by a timeout in the ffmpeg implementation, not by too few packets. That is
            // why there will be another try of calling avcodec_receive_frame
                qWarning() << "Unexpected FFmpeg behavior: EAGAIN state for avcodec_receive_frame "
                           << "at end of the stream";
                flushPacket = false;
                continue;
            }
            break;
        }

        if (receiveFrameResult < 0) {
            emit error(QMediaPlayer::FormatError, err2str(receiveFrameResult));
            break;
        }


        // Avoid starvation on FFmpeg decoders with fixed size frame pool
        if (m_trackType == QPlatformMediaPlayer::VideoStream)
            avFrame = copyFromHwPool(std::move(avFrame));

        if (m_trackType == QPlatformMediaPlayer::VideoStream
            && frameAuxiliaryId(avFrame.get()) == 2) {
            // a picture of the depth layer, after its picture
            m_depthLayer = true;
            holdDepth(std::move(avFrame), m_codecContext.stream());
            pairDepth(false);
            continue;
        }
        if (m_trackType == QPlatformMediaPlayer::VideoStream && depthExpected()) {
            holdPicture(std::move(avFrame));
            pairDepth(false);
            continue;
        }

        onFrameFound({ m_sessionCtx.offset, std::move(avFrame), m_codecContext, id() });
    }
    if (flushPacket)
        pairDepth(true);
}

bool StreamDecoder::depthExpected() const
{
    if (m_depthCodecContext || m_depthLayer)
        return true;
    // the decoder says whether the stream has a depth layer, whose pictures it outputs
    return decoderOutputsDepthLayer(m_codecContext.context());
}

void StreamDecoder::decodeDepth(const Packet &packet)
{
    AVCodecContext *context = m_depthCodecContext->context();
    int result = avcodec_send_packet(context, packet.isValid() ? packet.avPacket() : nullptr);
    if (result == AVERROR(EAGAIN)) {
        receiveDepthFrames();
        result = avcodec_send_packet(context, packet.isValid() ? packet.avPacket() : nullptr);
    }
    if (result == 0 || !packet.isValid())
        receiveDepthFrames();
    pairDepth(false);
}

void StreamDecoder::receiveDepthFrames()
{
    while (true) {
        auto avFrame = makeAVFrame();
        if (avcodec_receive_frame(m_depthCodecContext->context(), avFrame.get()) < 0)
            break;
        holdDepth(std::move(avFrame), m_depthCodecContext->stream());
    }
}

static qint64 frameTime(const AVFrame *frame, const AVStream *stream)
{
    const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
            ? frame->best_effort_timestamp
            : frame->pts;
    return pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                                 : av_rescale_q(pts, stream->time_base, AV_TIME_BASE_Q);
}

void StreamDecoder::holdPicture(AVFrameUPtr frame)
{
    const qint64 time = frameTime(frame.get(), m_codecContext.stream());
    m_heldPictures.push_back({ std::move(frame), time });
}

void StreamDecoder::holdDepth(AVFrameUPtr frame, const AVStream *stream)
{
    const qint64 time = frameTime(frame.get(), stream);
    m_heldDepths.push_back({ std::move(frame), time });
}

void StreamDecoder::pairDepth(bool final)
{
    // a depth map belongs to the picture of its time; a millisecond is the tolerance of streams
    // whose time bases differ
    constexpr qint64 tolerance = 1000;
    constexpr size_t maxHeldPictures = 4;
    while (!m_heldPictures.empty()) {
        DepthPending &picture = m_heldPictures.front();
        auto match =
                std::find_if(m_heldDepths.begin(), m_heldDepths.end(), [&](const DepthPending &d) {
                    return picture.time != AV_NOPTS_VALUE && d.time != AV_NOPTS_VALUE
                            && std::abs(d.time - picture.time) <= tolerance;
                });
        // frames without times (a raw stream): a depth map goes with the picture it follows, in
        // order
        if (match == m_heldDepths.end() && picture.time == AV_NOPTS_VALUE && !m_heldDepths.empty()
            && m_heldDepths.front().time == AV_NOPTS_VALUE)
            match = m_heldDepths.begin();
        AVFrameUPtr out;
        if (match != m_heldDepths.end()) {
            out = packPictureAndDepth(picture.frame.get(), match->frame.get());
            if (!out)
                qCWarning(qLcStreamDecoder) << "The picture could not be packed with its depth map";
            m_heldDepths.erase(m_heldDepths.begin(), match + 1);
        } else {
            // a depth map later than the picture: the picture's own is not coming
            const bool missed =
                    !m_heldDepths.empty() && m_heldDepths.back().time > picture.time + tolerance;
            if (!final && !missed && m_heldPictures.size() <= maxHeldPictures)
                break;
        }
        if (!out) { // the picture alone
            qCDebug(qLcStreamDecoder)
                    << "picture at" << picture.time
                    << "without its depth map, depth maps held:" << m_heldDepths.size()
                    << (m_heldDepths.empty() ? -1 : m_heldDepths.front().time);
            out = std::move(picture.frame);
        }
        m_heldPictures.pop_front();
        onFrameFound({ m_sessionCtx.offset, std::move(out), m_codecContext, id() });
    }
    // depth maps older than every picture to come have no picture any more
    while (!m_heldDepths.empty() && !m_heldPictures.empty()
           && m_heldDepths.front().time + tolerance < m_heldPictures.front().time)
        m_heldDepths.pop_front();
    // the decoders' delays differ (frame threads): depth maps may come well before their pictures
    constexpr size_t maxHeldDepths = 64;
    while (m_heldDepths.size() > maxHeldDepths)
        m_heldDepths.pop_front();
}

void StreamDecoder::clearDepth()
{
    m_heldPictures.clear();
    m_heldDepths.clear();
}

void StreamDecoder::decodeSubtitle(const Packet &packet)
{
    if (!packet.isValid())
        return;
    //    qCDebug(qLcDecoder) << "    decoding subtitle" << "has delay:" <<
    //    (codec->codec->capabilities & AV_CODEC_CAP_DELAY);
    AVSubtitle subtitle;
    memset(&subtitle, 0, sizeof(subtitle));
    int gotSubtitle = 0;

    const int res =
            avcodec_decode_subtitle2(m_codecContext.context(), &subtitle, &gotSubtitle, packet.avPacket());
    //    qCDebug(qLcDecoder) << "       subtitle got:" << res << gotSubtitle << subtitle.format <<
    //    Qt::hex << (quint64)subtitle.pts;
    if (res < 0 || !gotSubtitle)
        return;

    // apparently the timestamps in the AVSubtitle structure are not always filled in
    // if they are missing, use the packets pts and duration values instead
    TrackPosition start = 0, end = 0;
    if (subtitle.pts == AV_NOPTS_VALUE) {
        start = m_codecContext.toTrackPosition(AVStreamPosition(packet.avPacket()->pts));
        end = start + m_codecContext.toTrackDuration(AVStreamDuration(packet.avPacket()->duration));
    } else {
        auto pts = timeStampUs(subtitle.pts, AVRational{ 1, AV_TIME_BASE });
        start = TrackPosition(*pts + qint64(subtitle.start_display_time) * 1000);
        end = TrackPosition(*pts + qint64(subtitle.end_display_time) * 1000);
    }

    if (end <= start) {
        qWarning() << "Invalid subtitle time";
        return;
    }
    //        qCDebug(qLcDecoder) << "    got subtitle (" << start << "--" << end << "):";
    QString text;
    for (uint i = 0; i < subtitle.num_rects; ++i) {
        const auto *r = subtitle.rects[i];
        //            qCDebug(qLcDecoder) << "    subtitletext:" << r->text << "/" << r->ass;
        if (i)
            text += QLatin1Char('\n');
        if (r->text)
            text += QString::fromUtf8(r->text);
        else {
            const char *ass = r->ass;
            int nCommas = 0;
            while (*ass) {
                if (nCommas == 8)
                    break;
                if (*ass == ',')
                    ++nCommas;
                ++ass;
            }
            text += QString::fromUtf8(ass);
        }
    }
    text.replace(QLatin1String("\\N"), QLatin1String("\n"));
    text.replace(QLatin1String("\\n"), QLatin1String("\n"));
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    if (text.endsWith(QLatin1Char('\n')))
        text.chop(1);

    onFrameFound({ m_sessionCtx.offset, text, start, end - start, id() });

    // TODO: maybe optimize
    onFrameFound({ m_sessionCtx.offset, QString(), end, TrackDuration(0), id() });
}
} // namespace QFFmpeg

QT_END_NAMESPACE

#include "moc_qffmpegstreamdecoder_p.cpp"
