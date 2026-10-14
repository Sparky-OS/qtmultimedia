// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideostereo_p.h"
#ifdef QT_MM_STEREO_POLICY
#include <stereo-policy.h>
#endif

#include <QtCore/qmath.h>
#include <QtCore/qfloat16.h>

#include <algorithm>
#include <cmath>
#include <vector>

QT_BEGIN_NAMESPACE

namespace QQuickVideoStereo {

using Mode = QVideoFrameFormat::StereoMode;

bool hasTwoViews(Mode mode)
{
    return mode != Mode::None && mode != Mode::AnaglyphCyanRed && mode != Mode::AnaglyphGreenMagenta;
}

bool isFullSize(const Packing &packing, QSize frameSize)
{
    switch (packing.size) {
    case Packing::Size::Half:
        return false;
    case Packing::Size::Full:
        return true;
    case Packing::Size::ByShape:
        break;
    }
    // Nothing else says, so the shape does, by the one rule of Sparky Stereo OS's stereo policy; without it the shape
    // decides nothing
#ifdef QT_MM_STEREO_POLICY
    switch (packing.mode) {
    case Mode::SideBySide:
    case Mode::SideBySideQuincunx:
        return stereo_policy_video_views_by_shape(frameSize.width(), frameSize.height(), STEREO_POLICY_LAYOUT_SIDE_BY_SIDE) == 1;
    case Mode::TopBottom:
        return stereo_policy_video_views_by_shape(frameSize.width(), frameSize.height(), STEREO_POLICY_LAYOUT_TOP_AND_BOTTOM) == 1;
    default:
        return false;
    }
#else
    Q_UNUSED(frameSize)
    return false;
#endif
}

QSize viewSize(const Packing &packing, QSize frameSize)
{
    const bool full = isFullSize(packing, frameSize);
    switch (packing.mode) {
    case Mode::SideBySide:
    case Mode::SideBySideQuincunx:
        return full ? QSize(frameSize.width() / 2, frameSize.height()) : frameSize;
    case Mode::TopBottom:
        return full ? QSize(frameSize.width(), frameSize.height() / 2) : frameSize;
    default:
        return frameSize;
    }
}

bool viewRect(const Packing &packing, QSize frameSize, int index, QRect *rect)
{
    switch (packing.mode) {
    case Mode::SideBySide:
    case Mode::SideBySideQuincunx:
        *rect = QRect(index * (frameSize.width() / 2), 0, frameSize.width() / 2, frameSize.height());
        return true;
    case Mode::TopBottom:
        *rect = QRect(0, index * (frameSize.height() / 2), frameSize.width(), frameSize.height() / 2);
        return true;
    default:
        return false;
    }
}

namespace {

// The samples of a view along one axis: sample i of the packing is at the center
// stride * i + phase of the full resolution view, where stride 1 and phase 0.5 is a view
// that has that resolution already
struct Axis
{
    int source = 0;     // samples
    int target = 0;     // pixels at full resolution
    double stride = 1;
    double phase = 0.5;
};

double kernel(Scaling scaling, double x)
{
    x = std::abs(x);
    switch (scaling) {
    case Scaling::Bilinear:
        return x < 1 ? 1 - x : 0;
    case Scaling::Bicubic: { // Catmull-Rom
        if (x < 1)
            return 1.5 * x * x * x - 2.5 * x * x + 1;
        if (x < 2)
            return -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2;
        return 0;
    }
    case Scaling::Lanczos:
        if (x < 1e-9)
            return 1;
        if (x < 3) {
            const double px = M_PI * x;
            return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
        }
        return 0;
    }
    return 0;
}

double support(Scaling scaling)
{
    return scaling == Scaling::Bilinear ? 1 : scaling == Scaling::Bicubic ? 2 : 3;
}

struct Taps
{
    int first = 0;
    std::vector<float> weights;
};

// The weights that make target pixel t out of the samples: the kernel is at the source position
// of the center of t, and wider by the factor of a reduction
std::vector<Taps> makeTaps(const Axis &axis, Scaling scaling, double phaseOverride = -1)
{
    const double phase = phaseOverride >= 0 ? phaseOverride : axis.phase;
    std::vector<Taps> result(axis.target);
    const double scale = axis.stride;
    // reducing is when a target pixel covers more than one sample
    const double widen = std::max(1.0, 1.0 / scale);
    for (int t = 0; t < axis.target; ++t) {
        Taps &taps = result[t];
        const double u = ((t + 0.5) - phase) / scale; // source coordinate in samples, centers at integers
        if (scale == 1 && std::abs(phase - 0.5) < 1e-9) {
            taps.first = t < axis.source ? t : axis.source - 1;
            taps.weights = { 1.0f };
            continue;
        }
        const double radius = support(scaling) * widen;
        int i0 = int(std::floor(u - radius)) + 1;
        int i1 = int(std::floor(u + radius));
        taps.first = i0;
        double sum = 0;
        std::vector<double> w;
        for (int i = i0; i <= i1; ++i) {
            const double v = kernel(scaling, (i - u) / widen);
            w.push_back(v);
            sum += v;
        }
        // samples outside the view take the value of the edge: fold their weights in
        std::vector<float> folded;
        int lo = std::max(0, i0), hi = std::min(axis.source - 1, i1);
        if (hi < lo) {
            taps.first = std::clamp(i0, 0, axis.source - 1);
            taps.weights = { 1.0f };
            continue;
        }
        folded.assign(hi - lo + 1, 0.f);
        for (int i = i0; i <= i1; ++i) {
            const int c = std::clamp(i, 0, axis.source - 1);
            folded[c - lo] += float(w[i - i0] / sum);
        }
        taps.first = lo;
        taps.weights = std::move(folded);
    }
    return result;
}

} // namespace

QImage extractView(const QImage &frameImage, const Packing &packing, int index, Scaling scaling)
{
    const QImage src = frameImage.format() == QImage::Format_RGBA32FPx4_Premultiplied
            ? frameImage
            : frameImage.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    const QSize frameSize = src.size();
    // where the views are to end up: the size of a view of the frame as it is shown
    const QSize shown = packing.displaySize.isValid() ? packing.displaySize : frameSize;
    const QSize target = viewSize(packing, shown);
    if (src.isNull() || target.isEmpty())
        return {};

    const bool full = isFullSize(packing, shown);
    Q_UNUSED(full);
    const int first = index; // 0 or 1: the view that is first or second in the packing

    // The samples of the view in the frame, per axis
    int sx0 = 0, sy0 = 0;            // where the view starts in the frame
    Axis ax, ay;
    ax.target = target.width();
    ay.target = target.height();
    ax.source = ax.target;
    ay.source = ay.target;
    bool checker = false;
    // interleaved packings read every second row or column of the whole frame
    int rowStep = 1, colStep = 1, rowStart = 0, colStart = 0;

    switch (packing.mode) {
    case Mode::SideBySide:
    case Mode::SideBySideQuincunx:
        sx0 = first * (frameSize.width() / 2);
        ax.source = frameSize.width() / 2;
        ay.source = frameSize.height();
        break;
    case Mode::TopBottom:
        sy0 = first * (frameSize.height() / 2);
        ay.source = frameSize.height() / 2;
        ax.source = frameSize.width();
        break;
    case Mode::RowInterleaved:
        rowStep = 2;
        rowStart = first;
        ay.source = (frameSize.height() - first + 1) / 2;
        break;
    case Mode::ColumnInterleaved:
        colStep = 2;
        colStart = first;
        ax.source = (frameSize.width() - first + 1) / 2;
        break;
    case Mode::Checkerboard:
        checker = true;
        ax.source = (frameSize.width() + 1) / 2;
        break;
    default:
        break;
    }

    // strides and phases: a squeezed view has its samples twice as far apart, centered in their pairs
    auto setSqueezed = [](Axis &a) { a.stride = double(a.target) / a.source; a.phase = a.stride / 2; };
    switch (packing.mode) {
    case Mode::SideBySide:
    case Mode::SideBySideQuincunx:
        // the samples of a view are as far apart as the view is larger than they are many
        setSqueezed(ax);
        setSqueezed(ay);
        break;
    case Mode::TopBottom:
        setSqueezed(ax);
        setSqueezed(ay);
        break;
    case Mode::RowInterleaved:
        ay.stride = 2;
        ay.phase = first + 0.5;
        break;
    case Mode::ColumnInterleaved:
        ax.stride = 2;
        ax.phase = first + 0.5;
        break;
    case Mode::Checkerboard:
        ax.stride = 2;
        ax.phase = 0.5; // set per row below
        break;
    default:
        ax.stride = ay.stride = 1;
        break;
    }

    const bool scaleX = !(ax.stride == 1 && ax.source == ax.target && std::abs(ax.phase - 0.5) < 1e-9);
    const bool scaleY = !(ay.stride == 1 && ay.source == ay.target && std::abs(ay.phase - 0.5) < 1e-9);

    // 1. horizontal: a row of the target width for every row of the view's samples
    const int rows = ay.source;
    QImage mid(ax.target, rows, QImage::Format_RGBA32FPx4_Premultiplied);
    const std::vector<Taps> tapsX = scaleX && !checker ? makeTaps(ax, scaling) : std::vector<Taps>();
    std::vector<Taps> tapsXPhase[2];
    if (checker) {
        // the samples of the view are at x = 2 i + ((y + first) % 2) in the row y
        for (int p = 0; p < 2; ++p)
            tapsXPhase[p] = makeTaps(ax, scaling, p + 0.5);
    }
    for (int r = 0; r < rows; ++r) {
        const int sy = rowStep == 2 ? rowStart + 2 * r : sy0 + r;
        const float *line = reinterpret_cast<const float *>(src.constScanLine(std::min(sy, frameSize.height() - 1)));
        float *out = reinterpret_cast<float *>(mid.scanLine(r));
        if (!scaleX && !checker && colStep == 1) {
            std::copy(line + sx0 * 4, line + (sx0 + ax.target) * 4, out);
            continue;
        }
        if (checker) {
            // the row's own samples, gathered, then the taps of its phase
            const int phase = (sy + first) % 2;
            const std::vector<Taps> &taps = tapsXPhase[phase];
            for (int x = 0; x < ax.target; ++x) {
                const Taps &t = taps[x];
                float acc[4] = {0, 0, 0, 0};
                for (size_t k = 0; k < t.weights.size(); ++k) {
                    const int i = t.first + int(k);
                    const int px = std::min(2 * i + phase, frameSize.width() - 1);
                    const float *p = line + px * 4;
                    for (int c = 0; c < 4; ++c)
                        acc[c] += p[c] * t.weights[k];
                }
                std::copy(acc, acc + 4, out + x * 4);
            }
            continue;
        }
        const std::vector<Taps> &taps = tapsX;
        for (int x = 0; x < ax.target; ++x) {
            const Taps &t = taps[x];
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.weights.size(); ++k) {
                const int i = t.first + int(k);
                const int px = colStep == 2 ? std::min(colStart + 2 * i, frameSize.width() - 1)
                                            : sx0 + i;
                const float *p = line + px * 4;
                for (int c = 0; c < 4; ++c)
                    acc[c] += p[c] * t.weights[k];
            }
            std::copy(acc, acc + 4, out + x * 4);
        }
    }
    if (!scaleY)
        return mid;

    // 2. vertical
    const std::vector<Taps> tapsY = makeTaps(ay, scaling);
    QImage result(ax.target, ay.target, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < ay.target; ++y) {
        const Taps &t = tapsY[y];
        float *out = reinterpret_cast<float *>(result.scanLine(y));
        for (int x = 0; x < ax.target * 4; ++x) {
            float acc = 0;
            for (size_t k = 0; k < t.weights.size(); ++k)
                acc += reinterpret_cast<const float *>(mid.constScanLine(t.first + int(k)))[x] * t.weights[k];
            out[x] = acc;
        }
    }
    return result;
}

int bytesPerPixel(BufferFormat format)
{
    return format == BufferFormat::Xbgr16161616f || format == BufferFormat::Xbgr16161616 ? 8 : 4;
}

void writePair(const QImage &left, const QImage &right, BufferFormat format, uchar *dst, int stride)
{
    const int w = left.width();
    const int h = left.height();
    for (int y = 0; y < h; ++y) {
        uchar *line = dst + qsizetype(y) * stride;
        for (int e = 0; e < 2; ++e) {
            const float *in = reinterpret_cast<const float *>((e ? right : left).constScanLine(y));
            switch (format) {
            case BufferFormat::Xrgb8888: {
                quint32 *out = reinterpret_cast<quint32 *>(line) + e * w;
                for (int x = 0; x < w; ++x) {
                    const quint32 r = quint32(std::clamp(in[x * 4] * 255.f + 0.5f, 0.f, 255.f));
                    const quint32 g = quint32(std::clamp(in[x * 4 + 1] * 255.f + 0.5f, 0.f, 255.f));
                    const quint32 b = quint32(std::clamp(in[x * 4 + 2] * 255.f + 0.5f, 0.f, 255.f));
                    out[x] = 0xff000000u | (r << 16) | (g << 8) | b;
                }
                break;
            }
            case BufferFormat::Xrgb2101010: {
                quint32 *out = reinterpret_cast<quint32 *>(line) + e * w;
                for (int x = 0; x < w; ++x) {
                    const quint32 r = quint32(std::clamp(in[x * 4] * 1023.f + 0.5f, 0.f, 1023.f));
                    const quint32 g = quint32(std::clamp(in[x * 4 + 1] * 1023.f + 0.5f, 0.f, 1023.f));
                    const quint32 b = quint32(std::clamp(in[x * 4 + 2] * 1023.f + 0.5f, 0.f, 1023.f));
                    out[x] = (3u << 30) | (r << 20) | (g << 10) | b;
                }
                break;
            }
            case BufferFormat::Xbgr16161616: {
                quint16 *out = reinterpret_cast<quint16 *>(line) + e * w * 4;
                for (int x = 0; x < w; ++x) {
                    for (int c = 0; c < 3; ++c)
                        out[x * 4 + c] = quint16(std::clamp(in[x * 4 + c] * 65535.f + 0.5f, 0.f, 65535.f));
                    out[x * 4 + 3] = 0xffff;
                }
                break;
            }
            case BufferFormat::Xbgr16161616f: {
                qfloat16 *out = reinterpret_cast<qfloat16 *>(line) + e * w * 4;
                for (int x = 0; x < w; ++x) {
                    out[x * 4] = qfloat16(in[x * 4]);
                    out[x * 4 + 1] = qfloat16(in[x * 4 + 1]);
                    out[x * 4 + 2] = qfloat16(in[x * 4 + 2]);
                    out[x * 4 + 3] = qfloat16(1.f);
                }
                break;
            }
            }
        }
    }
}

} // namespace QQuickVideoStereo

QT_END_NAMESPACE
