// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideodepth_p.h"

#include <QtMultimedia/private/qvideoframeconverter_p.h>
#include <QtGui/qimage.h>

#ifdef QT_MM_STEREO_DIBR
#  include <stereo-dibr.h>
#endif

#include <cmath>
#include <cstring>
#include <vector>

QT_BEGIN_NAMESPACE

namespace QQuickVideoDepth {

using Mode = QVideoFrameFormat::StereoMode;
using PixelFormat = QVideoFrameFormat::PixelFormat;

bool isDepthLayout(Mode mode)
{
    switch (mode) {
    case Mode::DepthSideBySide:
    case Mode::DepthTopBottom:
    case Mode::DepthGraphics:
    case Mode::DepthBackground:
    case Mode::DepthSeparate:
        return true;
    default:
        return false;
    }
}

QRect pictureRect(Mode mode, bool depthFirst, QSize frameSize)
{
    const int w = frameSize.width(), h = frameSize.height();
    switch (mode) {
    case Mode::DepthSideBySide:
        return depthFirst ? QRect(w / 2, 0, w / 2, h) : QRect(0, 0, w / 2, h);
    case Mode::DepthTopBottom:
        return depthFirst ? QRect(0, h / 2, w, h / 2) : QRect(0, 0, w, h / 2);
    case Mode::DepthGraphics:
        return QRect(0, 0, w, h / 4);
    case Mode::DepthBackground:
        return QRect(0, 0, w / 2, h / 2);
    default:
        return QRect(0, 0, w, h);
    }
}

bool canDraw()
{
#ifdef QT_MM_STEREO_DIBR
    return true;
#else
    return false;
#endif
}

#ifdef QT_MM_STEREO_DIBR

namespace {

// Where a component of a pixel format is: the plane, its first sample and the samples from one
// to the next in that plane, and the subsampling against the first component
struct Component
{
    int plane = 0;
    int offset = 0;
    int step = 1;
    int log2w = 0;
    int log2h = 0;
    enum Kind { Luma, Chroma, Alpha, Color } kind = Luma;
};

struct Layout
{
    int count = 0;
    Component c[4];
    int bytes = 1; // per sample
    int bits = 8;
    int depthComponent = 0; // the component the depth samples are read from
    bool semiPlanar = false;
};

bool layoutOf(PixelFormat format, Layout *l)
{
    using C = Component;
    auto yuv = [&](int bytes, int bits, int log2w, int log2h, int uPlane, int vPlane) {
        *l = {};
        l->count = 3;
        l->bytes = bytes;
        l->bits = bits;
        l->c[0] = { 0, 0, 1, 0, 0, C::Luma };
        l->c[1] = { uPlane, 0, 1, log2w, log2h, C::Chroma };
        l->c[2] = { vPlane, 0, 1, log2w, log2h, C::Chroma };
    };
    auto semi = [&](int bytes, int bits, int uOffset, int vOffset) {
        *l = {};
        l->count = 3;
        l->bytes = bytes;
        l->bits = bits;
        l->semiPlanar = true;
        l->c[0] = { 0, 0, 1, 0, 0, C::Luma };
        l->c[1] = { 1, uOffset, 2, 1, 1, C::Chroma };
        l->c[2] = { 1, vOffset, 2, 1, 1, C::Chroma };
    };
    auto rgba = [&](int alphaOffset) {
        *l = {};
        l->count = 4;
        l->depthComponent = 1;
        for (int i = 0; i < 4; ++i)
            l->c[i] = { 0, i, 4, 0, 0, i == alphaOffset ? C::Alpha : C::Color };
        if (l->depthComponent == alphaOffset)
            l->depthComponent = 2;
    };
    switch (format) {
    case PixelFormat::Format_YUV420P:
        yuv(1, 8, 1, 1, 1, 2);
        return true;
    case PixelFormat::Format_YV12:
        yuv(1, 8, 1, 1, 2, 1);
        return true;
    case PixelFormat::Format_YUV422P:
        yuv(1, 8, 1, 0, 1, 2);
        return true;
    case PixelFormat::Format_YUV444P:
        yuv(1, 8, 0, 0, 1, 2);
        return true;
    case PixelFormat::Format_YUV420P10:
        yuv(2, 10, 1, 1, 1, 2);
        return true;
    case PixelFormat::Format_YUV422P16:
        yuv(2, 16, 1, 0, 1, 2);
        return true;
    case PixelFormat::Format_YUV444P16:
        yuv(2, 16, 0, 0, 1, 2);
        return true;
    case PixelFormat::Format_NV12:
        semi(1, 8, 0, 1);
        return true;
    case PixelFormat::Format_NV21:
        semi(1, 8, 1, 0);
        return true;
    case PixelFormat::Format_P010:
    case PixelFormat::Format_P016:
        semi(2, 16, 0, 1);
        return true;
    case PixelFormat::Format_Y8:
        *l = {};
        l->count = 1;
        l->c[0] = { 0, 0, 1, 0, 0, C::Luma };
        return true;
    case PixelFormat::Format_Y16:
        *l = {};
        l->count = 1;
        l->bytes = 2;
        l->bits = 16;
        l->c[0] = { 0, 0, 1, 0, 0, C::Luma };
        return true;
    // 32 bit RGB, in the byte order of memory (little endian for the 0xAARRGGBB formats)
    case PixelFormat::Format_ARGB8888:
    case PixelFormat::Format_ARGB8888_Premultiplied:
    case PixelFormat::Format_XRGB8888:
        rgba(3);
        return true;
    case PixelFormat::Format_BGRA8888:
    case PixelFormat::Format_BGRA8888_Premultiplied:
    case PixelFormat::Format_BGRX8888:
        rgba(0);
        return true;
    case PixelFormat::Format_ABGR8888:
    case PixelFormat::Format_XBGR8888:
    case PixelFormat::Format_RGBA8888:
    case PixelFormat::Format_RGBX8888:
        rgba(3);
        return true;
    default:
        return false;
    }
}

// A component of a mapped frame as a plane of its own, the samples copied out (a semi-planar or
// interleaved component) or used in place
struct PlaneData
{
    std::vector<uint8_t> copy;
    uint8_t *data = nullptr;
    ptrdiff_t stride = 0;
};

PlaneData componentPlane(uchar *const bits[4], const int strides[4], const Layout &l, int c, int w,
                         int h)
{
    const Component &comp = l.c[c];
    const int cw = (w + (1 << comp.log2w) - 1) >> comp.log2w;
    const int ch = (h + (1 << comp.log2h) - 1) >> comp.log2h;
    PlaneData p;
    if (comp.step == 1) {
        p.data = bits[comp.plane] + comp.offset * l.bytes;
        p.stride = strides[comp.plane];
        return p;
    }
    p.copy.resize(size_t(cw) * ch * l.bytes);
    p.stride = ptrdiff_t(cw) * l.bytes;
    for (int y = 0; y < ch; ++y) {
        const uint8_t *src =
                bits[comp.plane] + ptrdiff_t(y) * strides[comp.plane] + comp.offset * l.bytes;
        uint8_t *dst = p.copy.data() + ptrdiff_t(y) * p.stride;
        for (int x = 0; x < cw; ++x)
            std::memcpy(dst + x * l.bytes, src + ptrdiff_t(x) * comp.step * l.bytes, l.bytes);
    }
    p.data = p.copy.data();
    return p;
}

void putComponent(uchar *const bits[4], const int strides[4], const Layout &l, int c, int w, int h,
                  const PlaneData &p)
{
    const Component &comp = l.c[c];
    if (comp.step == 1 && p.copy.empty())
        return; // drawn in place
    const int cw = (w + (1 << comp.log2w) - 1) >> comp.log2w;
    const int ch = (h + (1 << comp.log2h) - 1) >> comp.log2h;
    for (int y = 0; y < ch; ++y) {
        const uint8_t *src = p.data + ptrdiff_t(y) * p.stride;
        uint8_t *dst =
                bits[comp.plane] + ptrdiff_t(y) * strides[comp.plane] + comp.offset * l.bytes;
        for (int x = 0; x < cw; ++x)
            std::memcpy(dst + ptrdiff_t(x) * comp.step * l.bytes, src + x * l.bytes, l.bytes);
    }
}

// A frame's components as a stereo_dibr_picture, the plane order of the components
void fillPicture(stereo_dibr_picture *pic, std::vector<PlaneData> &planes, const Layout &l, int w,
                 int h, bool fullRange)
{
    std::memset(pic, 0, sizeof(*pic));
    pic->width = w;
    pic->height = h;
    pic->nb_planes = l.count;
    const unsigned max = (1u << l.bits) - 1;
    for (int c = 0; c < l.count; ++c) {
        stereo_dibr_plane &pl = pic->plane[c];
        pl.data = planes[c].data;
        pl.stride = planes[c].stride;
        pl.bytes = l.bytes;
        pl.log2_w = l.c[c].log2w;
        pl.log2_h = l.c[c].log2h;
        // a hole of the none fill is black: limited range black and neutral chroma for YUV,
        // zero for RGB, opaque alpha
        switch (l.c[c].kind) {
        case Component::Luma:
            pl.hole = fullRange ? 0 : 16u << (l.bits - 8);
            break;
        case Component::Chroma:
            pl.hole = 1u << (l.bits - 1);
            break;
        case Component::Alpha:
            pl.hole = max;
            break;
        case Component::Color:
            pl.hole = 0;
            break;
        }
    }
}

int packingOf(Mode mode)
{
    switch (mode) {
    case Mode::DepthSideBySide:
        return STEREO_DIBR_PACKING_SIDE_BY_SIDE;
    case Mode::DepthTopBottom:
        return STEREO_DIBR_PACKING_TOP_BOTTOM;
    case Mode::DepthGraphics:
        return STEREO_DIBR_PACKING_GRAPHICS;
    case Mode::DepthBackground:
        return STEREO_DIBR_PACKING_BACKGROUND;
    default:
        return -1;
    }
}

} // namespace

struct Renderer::Private
{
    stereo_dibr *dibr = nullptr;
    std::vector<float> table, layerTable;
    std::vector<uint16_t> lut;
    ~Private() { stereo_dibr_free(dibr); }

    // 1: read, 0: malformed (one picture), -1: error
    int readMark(const QString &text, int bits, stereo_depth_convention *c)
    {
        uint16_t model[65];
        int nbModel = 0;
        const QByteArray mark = text.toUtf8();
        if (stereo_depth_read_mark(mark.constData(), c, model, &nbModel) < 0)
            return 0;
        c->bits = bits;
        if (nbModel) {
            lut.resize(size_t(1) << bits);
            if (stereo_depth_nonlinear_lut(model, nbModel, bits, lut.data()) < 0)
                return -1;
            c->lut = lut.data();
        }
        return 1;
    }

    // What a depth sample means: the user's mark, else the user's meaning, else the stream's mark,
    // else nearness; over the user's range, screen and shift
    int convention(const QString &fileMark, const Options &o, int bits, stereo_depth_convention *c)
    {
        std::memset(c, 0, sizeof(*c));
        c->bits = bits;
        c->zero = o.zero;
        c->range = o.range;
        c->shift = o.shift;
        c->kind = o.meaning < 0 ? STEREO_DEPTH_NEARNESS : o.meaning;
        if (o.zNear > 0 && o.zFar > 0) {
            c->z_present = 1;
            c->z_near = o.zNear;
            c->z_far = o.zFar;
        }
        if (!o.mark.isEmpty())
            return readMark(o.mark, bits, c);
        if (o.meaning >= 0)
            return 1;
        if (!fileMark.isEmpty())
            return readMark(fileMark, bits, c);
        return 1;
    }
};

Renderer::Renderer() : d(std::make_unique<Private>())
{
    d->dibr = stereo_dibr_alloc();
}

Renderer::~Renderer() = default;

Renderer::Result Renderer::draw(const QVideoFrame &input, Mode mode, bool depthFirst,
                                const QString &fileMark, const Options &options, QVideoFrame *out)
{
    const int packing = packingOf(mode);
    if (packing < 0 || !d->dibr || !input.isValid())
        return Result::Failed;

    QVideoFrame frame = input;
    Layout l;
    if (!layoutOf(frame.pixelFormat(), &l)) {
        // packed YUV and the formats of hardware textures: drawn from their 8 bit RGBA image
        const QImage image = qImageFromVideoFrame(frame, QVideoFrameImageDepth::Bits8);
        if (image.isNull())
            return Result::Failed;
        frame = QVideoFrame(image.convertToFormat(QImage::Format_RGBA8888));
        if (!layoutOf(frame.pixelFormat(), &l))
            return Result::Failed;
    }
    if (!frame.map(QVideoFrame::ReadOnly))
        return Result::Failed;

    const int w = frame.width(), h = frame.height();
    const bool fullRange = frame.surfaceFormat().colorRange() == QVideoFrameFormat::ColorRange_Full
            || l.c[0].kind == Component::Color;
    uchar *inBits[4] = {};
    int inStrides[4] = {};
    for (int p = 0; p < frame.planeCount() && p < 4; ++p) {
        inBits[p] = const_cast<uchar *>(frame.bits(p));
        inStrides[p] = frame.bytesPerLine(p);
    }
    std::vector<PlaneData> inPlanes;
    for (int c = 0; c < l.count; ++c)
        inPlanes.push_back(componentPlane(inBits, inStrides, l, c, w, h));
    stereo_dibr_picture whole;
    fillPicture(&whole, inPlanes, l, w, h, fullRange);
    // the depth samples are the first plane of a picture of the depth component alone
    stereo_dibr_picture depthWhole = whole;
    depthWhole.nb_planes = 1;
    depthWhole.plane[0] = whole.plane[l.depthComponent];

    stereo_dibr_parts parts, depthParts;
    if (stereo_dibr_unpack(&whole, packing, depthFirst, 0, &parts) < 0
        || stereo_dibr_unpack(&depthWhole, packing, depthFirst, 0, &depthParts) < 0) {
        frame.unmap();
        return Result::Failed;
    }
    const int vw = parts.picture.width, vh = parts.picture.height;

    stereo_depth_convention c;
    const int known = d->convention(fileMark, options, l.bits, &c);
    if (known <= 0) {
        frame.unmap();
        return known == 0 ? Result::PictureAlone : Result::Failed;
    }
    const size_t tableSize = size_t(1) << l.bits;
    d->table.resize(tableSize);
    if (stereo_depth_parallax_table(&c, vw, d->table.data()) < 0) {
        frame.unmap();
        return Result::Failed;
    }

    // the output: the same pixel format and color, twice the picture's width
    QVideoFrameFormat format = frame.surfaceFormat();
    format.setFrameSize(QSize(2 * vw, vh));
    format.setViewport(QRect(0, 0, 2 * vw, vh));
    format.setStereoMode(Mode::SideBySide);
    format.setStereoFlags({});
    format.setStereoDepthMark({});
    QVideoFrame result(format);
    if (!result.map(QVideoFrame::WriteOnly)) {
        frame.unmap();
        return Result::Failed;
    }
    uchar *outBits[4] = {};
    int outStrides[4] = {};
    for (int p = 0; p < result.planeCount() && p < 4; ++p) {
        outBits[p] = result.bits(p);
        outStrides[p] = result.bytesPerLine(p);
    }
    std::vector<PlaneData> outPlanes;
    for (int cc = 0; cc < l.count; ++cc)
        outPlanes.push_back(componentPlane(outBits, outStrides, l, cc, 2 * vw, vh));
    stereo_dibr_picture outWhole;
    fillPicture(&outWhole, outPlanes, l, 2 * vw, vh, fullRange);
    // the left and right views: the halves of the output
    stereo_dibr_picture left = outWhole, right = outWhole;
    left.width = right.width = vw;
    for (int cc = 0; cc < l.count; ++cc) {
        const int cx = (vw + (1 << l.c[cc].log2w) - 1) >> l.c[cc].log2w;
        right.plane[cc].data =
                static_cast<uint8_t *>(outWhole.plane[cc].data) + ptrdiff_t(cx) * l.bytes;
    }

    stereo_dibr_params prm;
    stereo_dibr_params_default(&prm);
    prm.view = options.view;
    prm.warp = options.warp;
    prm.fill = options.fill;
    prm.smooth = options.smooth;
    prm.sigma_h = options.sigmaH;
    prm.sigma_v = options.sigmaV;
    prm.radius = options.radius;
    int r;
    if (parts.layer.width) {
        // a layer's 0 is nothing there (docs/depth-marks.md)
        d->layerTable = d->table;
        d->layerTable[0] = NAN;
        r = stereo_dibr_render_layer(d->dibr, &prm, d->table.data(), int(tableSize), &parts.picture,
                                     &depthParts.depth, parts.layer_role, &parts.layer,
                                     d->layerTable.data(), int(tableSize), &depthParts.layer_depth,
                                     &left, &right);
    } else {
        r = stereo_dibr_render(d->dibr, &prm, d->table.data(), int(tableSize), &parts.picture,
                               &depthParts.depth, &left, &right);
    }
    for (int cc = 0; cc < l.count; ++cc)
        putComponent(outBits, outStrides, l, cc, 2 * vw, vh, outPlanes[cc]);
    result.unmap();
    frame.unmap();
    if (r < 0)
        return Result::Failed;
    result.setStartTime(input.startTime());
    result.setEndTime(input.endTime());
    *out = result;
    return Result::Drawn;
}

#else

struct Renderer::Private
{
};

Renderer::Renderer() : d(std::make_unique<Private>()) { }

Renderer::~Renderer() = default;

Renderer::Result Renderer::draw(const QVideoFrame &, Mode, bool, const QString &, const Options &,
                                QVideoFrame *)
{
    return Result::Failed;
}

#endif

} // namespace QQuickVideoDepth

QT_END_NAMESPACE
