// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qffmpegdepth_p.h"

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/stereo3d.h>
#if __has_include(<libavutil/multiview.h>)
#  include <libavutil/multiview.h>
#  define QT_FFMPEG_DEPTH
#endif
}

#include <cstring>

QT_BEGIN_NAMESPACE

namespace QFFmpeg {

namespace {

constexpr const char *layoutKey = "STEREO3D_LAYOUT";
constexpr const char *depthKey = "STEREO3D_DEPTH";

// a number as the marks write them: the C notation, whatever the locale
QByteArray number(double value)
{
    return QByteArray::number(value, 'g', 15);
}

#ifdef QT_FFMPEG_DEPTH
const AVFrameSideData *sideData(const AVFrame *frame, AVFrameSideDataType type)
{
    return frame ? av_frame_get_side_data(frame, type) : nullptr;
}
#endif

int planeHeight(const AVPixFmtDescriptor *desc, int plane, int height)
{
    const bool chroma = !(desc->flags & AV_PIX_FMT_FLAG_RGB) && (plane == 1 || plane == 2);
    return chroma ? AV_CEIL_RSHIFT(height, desc->log2_chroma_h) : height;
}

// The software frame of a frame that may be in hardware memory
AVFrameUPtr softwareFrame(const AVFrame *frame)
{
    AVFrameUPtr sw = makeAVFrame();
    if (!sw)
        return {};
    if (frame->hw_frames_ctx) {
        if (av_hwframe_transfer_data(sw.get(), frame, 0) < 0)
            return {};
        av_frame_copy_props(sw.get(), frame);
    } else if (av_frame_ref(sw.get(), frame) < 0) {
        return {};
    }
    return sw;
}

} // namespace

bool depthSupported()
{
#ifdef QT_FFMPEG_DEPTH
    return true;
#else
    return false;
#endif
}

bool depthWanted()
{
    static const bool wanted = depthSupported()
            && !(qEnvironmentVariableIsSet("QT_FFMPEG_VIDEO_DEPTH")
                 && qEnvironmentVariableIntValue("QT_FFMPEG_VIDEO_DEPTH") == 0);
    return wanted;
}

int streamAuxiliaryId(const AVStream *stream)
{
#ifdef QT_FFMPEG_DEPTH
    if (!stream)
        return 0;
    if (const AVPacketSideData *sd = streamSideData(stream, AV_PKT_DATA_AUXILIARY_ID)) {
        if (sd->size >= sizeof(int))
            return *reinterpret_cast<const int *>(sd->data);
    }
    // a parallax map (ISO/IEC 23002-3) gives the views as a depth map does
    if (streamSideData(stream, AV_PKT_DATA_AUXILIARY_VIDEO_INFO))
        return 2;
#else
    Q_UNUSED(stream);
#endif
    return 0;
}

bool streamIsParallaxMap(const AVStream *stream)
{
#ifdef QT_FFMPEG_DEPTH
    if (const AVPacketSideData *sd = streamSideData(stream, AV_PKT_DATA_AUXILIARY_VIDEO_INFO)) {
        const auto *info = reinterpret_cast<const AVAuxiliaryVideoInfo *>(sd->data);
        return sd->size >= sizeof(AVAuxiliaryVideoInfo)
                && info->type == AV_AUXILIARY_VIDEO_PARALLAX;
    }
#else
    Q_UNUSED(stream);
#endif
    return false;
}

int frameAuxiliaryId(const AVFrame *frame)
{
#ifdef QT_FFMPEG_DEPTH
    if (const AVFrameSideData *sd = sideData(frame, AV_FRAME_DATA_AUXILIARY_ID)) {
        if (sd->size >= sizeof(int))
            return *reinterpret_cast<const int *>(sd->data);
    }
#else
    Q_UNUSED(frame);
#endif
    return 0;
}

bool decoderOutputsDepthLayer(AVCodecContext *context)
{
    if (!context)
        return false;
    int64_t available = 0;
    int64_t output = 0;
    constexpr int search = AV_OPT_SEARCH_CHILDREN;
    return av_opt_get_int(context, "depth_available", search, &available) >= 0
            && av_opt_get_int(context, "depth_output", search, &output) >= 0 && available && output;
}

std::optional<DepthLayout> depthLayoutFromWord(QByteArrayView word)
{
    using Mode = QVideoFrameFormat::StereoMode;
    using Flag = QVideoFrameFormat::StereoFlag;
    if (word.isEmpty())
        return std::nullopt;
    // docs/depth-marks.md, Stereo3DLayout
    struct Entry
    {
        const char *word;
        Mode mode;
        bool depthFirst;
    };
    static const Entry entries[] = {
        { "depthSideBySidePictureFirst", Mode::DepthSideBySide, false },
        { "depthSideBySideDepthFirst", Mode::DepthSideBySide, true },
        { "depthTopBottomPictureFirst", Mode::DepthTopBottom, false },
        { "depthTopBottomDepthFirst", Mode::DepthTopBottom, true },
        { "depthGraphicsTopBottom", Mode::DepthGraphics, false },
        { "depthBackgroundQuarters", Mode::DepthBackground, false },
        { "depthSeparate", Mode::DepthSeparate, false },
    };
    for (const Entry &e : entries) {
        if (word == QByteArrayView(e.word)) {
            DepthLayout result;
            result.mode = e.mode;
            if (e.depthFirst)
                result.flags |= Flag::DepthFirst;
            return result;
        }
    }
    return std::nullopt;
}

StreamDepth streamDepth(const AVStream *stream)
{
    StreamDepth result;
    if (!stream)
        return result;
    if (const AVDictionaryEntry *tag = av_dict_get(stream->metadata, layoutKey, nullptr, 0))
        result.layout = depthLayoutFromWord(QByteArrayView(tag->value));
    if (const AVDictionaryEntry *tag = av_dict_get(stream->metadata, depthKey, nullptr, 0))
        result.mark = QString::fromUtf8(tag->value);
    return result;
}

StreamDepth frameDepth(const AVFrame *frame)
{
    StreamDepth result;
    if (!frame)
        return result;
    if (const AVDictionaryEntry *tag = av_dict_get(frame->metadata, layoutKey, nullptr, 0))
        result.layout = depthLayoutFromWord(QByteArrayView(tag->value));
    if (const AVDictionaryEntry *tag = av_dict_get(frame->metadata, depthKey, nullptr, 0))
        result.mark = QString::fromUtf8(tag->value);
    return result;
}

QString depthMarkFromSideData(const AVFrame *depth, const AVFrame *picture, int viewWidth)
{
#ifdef QT_FFMPEG_DEPTH
    // H.264 H.13.2.3, H.265 G.14.3.4: the first view's parameters
    const AVFrameSideData *sd = sideData(depth, AV_FRAME_DATA_DEPTH_REPRESENTATION_INFO);
    if (!sd)
        sd = sideData(picture, AV_FRAME_DATA_DEPTH_REPRESENTATION_INFO);
    if (sd) {
        const auto *info = reinterpret_cast<const AVDepthRepresentationInfo *>(sd->data);
        const AVDepthRepresentationView *v =
                info->nb_views ? av_depth_representation_info_get_view(info, 0) : nullptr;
        QByteArray mark;
        switch (info->type) {
        case AV_DEPTH_REPRESENTATION_INVERSE_Z:
            mark = "inverseZ";
            break;
        case AV_DEPTH_REPRESENTATION_DISPARITY:
            mark = "disparity";
            break;
        case AV_DEPTH_REPRESENTATION_Z:
            mark = "z";
            break;
        case AV_DEPTH_REPRESENTATION_NONLINEAR_DISPARITY:
            mark = "nonlinearDisparity";
            break;
        default:
            return {}; // a reserved type says nothing
        }
        if (v && info->z_near_present && info->z_far_present
            && (info->type == AV_DEPTH_REPRESENTATION_INVERSE_Z
                || info->type == AV_DEPTH_REPRESENTATION_Z)) {
            mark += " near=" + number(av_multiview_real_to_double(&v->z_near));
            mark += " far=" + number(av_multiview_real_to_double(&v->z_far));
        }
        if (v && info->d_min_present && info->d_max_present
            && (info->type == AV_DEPTH_REPRESENTATION_DISPARITY
                || info->type == AV_DEPTH_REPRESENTATION_NONLINEAR_DISPARITY)) {
            // in luma samples of the view (the picture)
            mark += " min=" + number(av_multiview_real_to_double(&v->d_min));
            mark += " max=" + number(av_multiview_real_to_double(&v->d_max));
            mark += " width=" + QByteArray::number(viewWidth);
        }
        if (info->type == AV_DEPTH_REPRESENTATION_NONLINEAR_DISPARITY
            && info->nb_nonlinear_model >= 3) {
            mark += " model=";
            for (int i = 0; i < info->nb_nonlinear_model; ++i)
                mark += (i ? "," : "") + QByteArray::number(info->nonlinear_model[i]);
        }
        return QString::fromLatin1(mark);
    }
    // ISO/IEC 23002-3, which is not freely available: as open implementations read it
    sd = sideData(depth, AV_FRAME_DATA_AUXILIARY_VIDEO_INFO);
    if (sd) {
        const auto *info = reinterpret_cast<const AVAuxiliaryVideoInfo *>(sd->data);
        if (info->type == AV_AUXILIARY_VIDEO_PARALLAX) {
            return QString::fromLatin1("parallax parallaxZero=" + number(info->parallax_zero)
                                       + " parallaxScale=" + number(info->parallax_scale)
                                       + " wref=" + number(info->wref));
        }
        // kfar behind the screen, knear in front of it: the screen is where one gives way to the
        // other
        if (info->nkfar + info->nknear > 0)
            return QString::fromLatin1(
                    "nearness zero=" + number(double(info->nkfar) / (info->nkfar + info->nknear)));
        return QStringLiteral("nearness");
    }
#else
    Q_UNUSED(depth);
    Q_UNUSED(picture);
    Q_UNUSED(viewWidth);
#endif
    return {};
}

AVFrameUPtr packPictureAndDepth(const AVFrame *picture, const AVFrame *depth)
{
    if (!picture || !depth)
        return {};
    AVFrameUPtr pic = softwareFrame(picture);
    AVFrameUPtr dep = softwareFrame(depth);
    if (!pic || !dep)
        return {};
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(AVPixelFormat(pic->format));
    const AVPixFmtDescriptor *ddesc = av_pix_fmt_desc_get(AVPixelFormat(dep->format));
    if (!desc || !ddesc || !(desc->flags & AV_PIX_FMT_FLAG_PLANAR)
        || (desc->flags
            & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL
               | AV_PIX_FMT_FLAG_FLOAT))
        || (ddesc->flags
            & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL
               | AV_PIX_FMT_FLAG_FLOAT))
        || desc->comp[0].step > 2 || ddesc->comp[0].step > 2)
        return {};

    const int w = pic->width, h = pic->height;
    AVFrameUPtr out = makeAVFrame();
    out->format = pic->format;
    out->width = 2 * w;
    out->height = h;
    if (av_frame_get_buffer(out.get(), 0) < 0 || av_frame_copy_props(out.get(), pic.get()) < 0)
        return {};
    av_frame_remove_side_data(out.get(), AV_FRAME_DATA_STEREO3D);

    // the left half: the picture
    const int planes = av_pix_fmt_count_planes(AVPixelFormat(pic->format));
    for (int p = 0; p < planes; ++p) {
        const int bytes = av_image_get_linesize(AVPixelFormat(pic->format), w, p);
        if (bytes <= 0)
            return {};
        av_image_copy_plane(out->data[p], out->linesize[p], pic->data[p], pic->linesize[p], bytes,
                            planeHeight(desc, p, h));
    }

    // the right half: the depth samples as the luma, at the picture's size and depth; the other
    // components neutral (mid chroma, opaque alpha, gray for RGB)
    const AVComponentDescriptor &luma = desc->comp[0];
    const AVComponentDescriptor &dluma = ddesc->comp[0];
    const int bits = luma.depth, dbits = dluma.depth;
    const bool rgb = desc->flags & AV_PIX_FMT_FLAG_RGB;
    if (dbits < 1 || dbits > 16)
        return {};
    // a component deeper than 8 bits is in 16 bit words
    auto put = [&](const AVComponentDescriptor &comp, int x, int y, unsigned value) {
        uint8_t *row =
                out->data[comp.plane] + ptrdiff_t(y) * out->linesize[comp.plane] + comp.offset;
        if (comp.depth > 8)
            reinterpret_cast<uint16_t *>(row)[x * comp.step / 2] = uint16_t(value << comp.shift);
        else
            row[x * comp.step] = uint8_t(value << comp.shift);
    };
    auto get = [](const AVComponentDescriptor &comp, const uint8_t *row, int x) {
        row += comp.offset;
        const unsigned s = comp.depth > 8
                ? reinterpret_cast<const uint16_t *>(row)[x * comp.step / 2]
                : row[x * comp.step];
        return (s >> comp.shift) & ((1u << comp.depth) - 1);
    };
    const uint64_t maxOut = (1u << bits) - 1, maxIn = (1u << dbits) - 1;
    for (int y = 0; y < h; ++y) {
        const int sy = int((int64_t(y) * dep->height) / h);
        const uint8_t *drow = dep->data[dluma.plane] + ptrdiff_t(sy) * dep->linesize[dluma.plane];
        for (int x = 0; x < w; ++x) {
            const int sx = int((int64_t(x) * dep->width) / w);
            // the same fraction of the largest value, in the picture's bits
            const uint64_t s = get(dluma, drow, sx);
            put(luma, w + x, y, unsigned((s * maxOut + maxIn / 2) / maxIn));
        }
    }
    for (int c = 1; c < desc->nb_components; ++c) {
        const AVComponentDescriptor &comp = desc->comp[c];
        const bool alpha = c == 3 || (c == 1 && desc->nb_components == 2);
        const bool full = alpha || rgb;
        const int cw = full ? w : AV_CEIL_RSHIFT(w, desc->log2_chroma_w);
        const int ch = full ? h : AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
        for (int y = 0; y < ch; ++y) {
            const uint8_t *lumaRow =
                    out->data[luma.plane] + ptrdiff_t(y) * out->linesize[luma.plane];
            for (int x = 0; x < cw; ++x) {
                const unsigned value = alpha ? (1u << comp.depth) - 1
                        : rgb ? get(luma, lumaRow, w + x) // the gray of the depth sample
                              : 1u << (comp.depth - 1);
                put(comp, cw + x, y, value);
            }
        }
    }

    av_dict_set(&out->metadata, layoutKey, "depthSideBySidePictureFirst", 0);
    const QString mark = depthMarkFromSideData(depth, picture, w);
    if (!mark.isEmpty())
        av_dict_set(&out->metadata, depthKey, mark.toUtf8().constData(), 0);
    return out;
}

} // namespace QFFmpeg

QT_END_NAMESPACE
