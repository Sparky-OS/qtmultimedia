// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideodepth_p.h"

#include <QtMultimedia/private/qmultimediautils_p.h>
#include <QtMultimedia/private/qvideoframeconverter_p.h>
#include <QtMultimedia/private/qvideotransformation_p.h>
#include <QtMultimedia/private/qhwvideobuffer_p.h>
#include <QtMultimedia/private/qvideoframe_p.h>
#include <QtGui/qimage.h>

#include <QtCore/qloggingcategory.h>
#include <QtCore/qthread.h>
#include <QtGui/qtguiglobal.h>

#ifdef QT_MM_STEREO_DIBR
#  include <stereo-dibr.h>
#  include <stereo-dibr-gl.h>
#  if QT_CONFIG(opengl)
#    include <QtGui/qopenglcontext.h>
#    include <QtGui/qopenglcontext_platform.h>
#    include <rhi/qrhi.h>
#    include <rhi/qrhi_platform.h>
#    include <QtGui/qopenglextrafunctions.h>
#    include <QtGui/qoffscreensurface.h>
#    include <QtGui/qsurfaceformat.h>
#    define QT_MM_DEPTH_GPU
#  endif
#endif

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

QT_BEGIN_NAMESPACE

Q_STATIC_LOGGING_CATEGORY(qLcVideoDepth, "qt.multimedia.video.depth")

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

Options::Options()
{
#ifdef QT_MM_STEREO_DIBR
    // the library's one default
    stereo_dibr_options o;
    stereo_dibr_options_default(&o);
    view = o.view;
    warp = o.warp;
    fill = o.fill;
    smooth = o.smooth;
    sigmaH = o.sigma_h;
    sigmaV = o.sigma_v;
    radius = o.radius;
    range = o.strength;
    zero = o.zero;
    shift = o.shift;
    automatic = o.automatic != 0;
    // the decoder's frames are imported where they are, unless QT_MM_DEPTH_COPY_FRAMES asks for the copy to system memory
    importFrames = !qEnvironmentVariableIntValue("QT_MM_DEPTH_COPY_FRAMES");
    // the drawn views go to the sink as textures, unless QT_MM_DEPTH_COPY_VIEWS asks for the read back to system memory
    viewsAsTexture = !qEnvironmentVariableIntValue("QT_MM_DEPTH_COPY_VIEWS");
#endif
}

bool Choice::operator==(const Choice &o) const
{
    return valid == o.valid && warp == o.warp && fill == o.fill && keepsUp == o.keepsUp
            && qFuzzyCompare(fps + 1, o.fps + 1) && skippedWarp == o.skippedWarp
            && skippedFill == o.skippedFill && onGpu == o.onGpu && gpuLost == o.gpuLost && gpu == o.gpu && skippedMs == o.skippedMs;
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

// The samples of a plane as a frame's transformation turns them for display: rotated clockwise, then
// mirrored horizontally (QVideoFrame::rotation() and mirrored(), as the scene graph applies them)
struct TurnedPlane
{
    std::vector<uint8_t> data;
    ptrdiff_t stride = 0;
    int width = 0, height = 0; // in samples
};

template <typename Sample>
void turnSamples(const uint8_t *src, ptrdiff_t stride, int w, int h, int rotation, bool mirrored,
                 TurnedPlane *out)
{
    for (int y = 0; y < out->height; ++y) {
        auto *dst = reinterpret_cast<Sample *>(out->data.data() + ptrdiff_t(y) * out->stride);
        for (int x = 0; x < out->width; ++x) {
            const int xr = mirrored ? out->width - 1 - x : x;
            int sx = xr, sy = y;
            switch (rotation) {
            case 90:
                sx = y;
                sy = h - 1 - xr;
                break;
            case 180:
                sx = w - 1 - xr;
                sy = h - 1 - y;
                break;
            case 270:
                sx = w - 1 - y;
                sy = xr;
                break;
            default:
                break;
            }
            dst[x] = *reinterpret_cast<const Sample *>(src + ptrdiff_t(sy) * stride
                                                       + ptrdiff_t(sx) * sizeof(Sample));
        }
    }
}

TurnedPlane turnPlane(const void *src, ptrdiff_t stride, int bytes, int w, int h,
                      const VideoTransformation &t)
{
    const int rotation = qToUnderlying(t.rotation);
    TurnedPlane out;
    out.width = rotation % 180 ? h : w;
    out.height = rotation % 180 ? w : h;
    out.stride = ptrdiff_t(out.width) * bytes;
    out.data.resize(size_t(out.stride) * size_t(out.height));
    const auto *bytesIn = static_cast<const uint8_t *>(src);
    if (bytes == 4)
        turnSamples<uint32_t>(bytesIn, stride, w, h, rotation, t.mirroredHorizontallyAfterRotation, &out);
    else if (bytes == 2)
        turnSamples<uint16_t>(bytesIn, stride, w, h, rotation, t.mirroredHorizontallyAfterRotation, &out);
    else
        turnSamples<uint8_t>(bytesIn, stride, w, h, rotation, t.mirroredHorizontallyAfterRotation, &out);
    return out;
}

// A picture turned, every plane into storage kept in `keep` (a quarter turn swaps the subsampling
// of the two axes with the axes)
stereo_dibr_picture turnPicture(const stereo_dibr_picture &in, const VideoTransformation &t,
                                std::vector<TurnedPlane> *keep)
{
    stereo_dibr_picture out = in;
    const bool quarter = qToUnderlying(t.rotation) % 180 != 0;
    out.width = quarter ? in.height : in.width;
    out.height = quarter ? in.width : in.height;
    for (int p = 0; p < in.nb_planes; ++p) {
        const stereo_dibr_plane &pl = in.plane[p];
        const int pw = (in.width + (1 << pl.log2_w) - 1) >> pl.log2_w;
        const int ph = (in.height + (1 << pl.log2_h) - 1) >> pl.log2_h;
        keep->push_back(turnPlane(pl.data, pl.stride, pl.bytes, pw, ph, t));
        out.plane[p].data = keep->back().data.data();
        out.plane[p].stride = keep->back().stride;
        if (quarter) {
            out.plane[p].log2_w = pl.log2_h;
            out.plane[p].log2_h = pl.log2_w;
        }
    }
    return out;
}

stereo_dibr_depth turnDepth(const stereo_dibr_depth &in, const VideoTransformation &t,
                            std::vector<TurnedPlane> *keep)
{
    keep->push_back(turnPlane(in.data, in.stride, in.bytes, in.width, in.height, t));
    stereo_dibr_depth out = in;
    out.data = keep->back().data.data();
    out.stride = keep->back().stride;
    out.width = keep->back().width;
    out.height = keep->back().height;
    return out;
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

#ifdef QT_MM_DEPTH_GPU

// ---- drawing on the GPU ----------------------------------------------------------------------
// libstereo-dibr's GLSL passes (stereo-dibr-gl.h) in an OpenGL context of its own, on the GPU Qt
// draws with (the one the platform's EGL or GLX gives this process: the Graphics page of the
// edition chooses it for players). The frame's planes are uploaded as they are, a pass here
// turns them into the RGBA picture the library reads, the library draws both views, and the full
// side by side frame is read back into the frame the player shows.

thread_local QOpenGLContext *g_depthContext = nullptr;

stereo_dibr_gl_function resolveGl(const char *name)
{
    return g_depthContext ? reinterpret_cast<stereo_dibr_gl_function>(g_depthContext->getProcAddress(name))
                          : nullptr;
}

constexpr unsigned GL_R8_ = 0x8229, GL_RG8_ = 0x822B, GL_RGBA8_ = 0x8058, GL_RED_ = 0x1903,
                   GL_RG_ = 0x8227, GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401,
                   GL_TEXTURE_2D_ = 0x0DE1, GL_UNPACK_ROW_LENGTH_ = 0x0CF2,
                   GL_PACK_ROW_LENGTH_ = 0x0D02, GL_FRAMEBUFFER_ = 0x8D40,
                   GL_COLOR_ATTACHMENT0_ = 0x8CE0, GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5, GL_R16_ = 0x822A,
                   GL_RG16_ = 0x822C, GL_RGBA16_ = 0x805B, GL_R16UI_ = 0x8234, GL_RED_INTEGER_ = 0x8D94,
                   GL_UNSIGNED_SHORT_ = 0x1403;

const char kVertex[] = R"(
const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() { gl_Position = vec4(p[gl_VertexID], 0.0, 1.0); }
)";

// The planes of a picture into RGBA: planar (three planes), semi-planar (Y and interleaved UV) or
// RGBA as it is stored. BT.601, BT.709 and BT.2020 matrices (Kr, Kb), limited or full range.
const char kConvert[] = R"(
uniform sampler2D u_y;
uniform sampler2D u_u;
uniform sampler2D u_v;
uniform int u_mode;       // 0 planar, 1 semi-planar UV, 2 semi-planar VU
uniform ivec2 u_shift;    // the chroma planes' subsampling, log2 of width and height
uniform vec2 u_k;         // Kr, Kb
uniform int u_full;
// what a sample of the texture is as a code of the source: 8 bit textures hold code / 255, 16 bit ones code / 65535 (the
// word); u_div turns a word into the code of an 8 bit video (4 for 10 bits in the low bits, 256 for 10 bits in the high
// bits and for 16 bits), u_max is the largest word of full range, u_mid the word of a chroma of 0
uniform float u_scale;
uniform float u_div;
uniform float u_max;
uniform float u_mid;
uniform ivec2 u_origin;   // where the picture starts in the planes (a frame the decoder keeps on the GPU is read in place)
out vec4 o_color;
void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy) + u_origin;
    float y = texelFetch(u_y, at, 0).r * u_scale;
    ivec2 c = ivec2(at.x >> u_shift.x, at.y >> u_shift.y);
    float u, v;
    if (u_mode == 0) {
        u = texelFetch(u_u, c, 0).r * u_scale;
        v = texelFetch(u_v, c, 0).r * u_scale;
    } else {
        vec2 uv = texelFetch(u_u, c, 0).rg * u_scale;
        u = u_mode == 1 ? uv.r : uv.g;
        v = u_mode == 1 ? uv.g : uv.r;
    }
    float yy, cb, cr;
    if (u_full != 0) {
        yy = y / u_max;
        cb = (u - u_mid) / u_max;
        cr = (v - u_mid) / u_max;
    } else {
        yy = (y / u_div - 16.0) / 219.0;
        cb = (u / u_div - 128.0) / 224.0;
        cr = (v / u_div - 128.0) / 224.0;
    }
    float r = yy + 2.0 * (1.0 - u_k.x) * cr;
    float b = yy + 2.0 * (1.0 - u_k.y) * cb;
    float g = (yy - u_k.x * r - u_k.y * b) / (1.0 - u_k.x - u_k.y);
    o_color = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
)";

// The views (RGBA, unorm) back to the source's planes at the source's depth: the luma of each pixel, or the chroma of the mean
// of the block of pixels a chroma sample covers; plane 0 Y, 1 U, 2 V, 3 U and V interleaved. The same codes as kConvert.
const char kEncode[] = R"(
uniform sampler2D u_target;
uniform int u_plane;
uniform ivec2 u_shift;
uniform ivec2 u_size;
uniform vec2 u_k;
uniform int u_full;
uniform float u_div;
uniform float u_max;
uniform float u_mid;
out vec4 o_color;
vec3 rgbAt(ivec2 p) { return texelFetch(u_target, clamp(p, ivec2(0), u_size - 1), 0).rgb; }
float luma(vec3 c) { return u_k.x * c.r + (1.0 - u_k.x - u_k.y) * c.g + u_k.y * c.b; }
float word_y(float yy) { return u_full != 0 ? yy * u_max : (16.0 + 219.0 * yy) * u_div; }
float word_c(float c) { return u_full != 0 ? u_mid + c * u_max : (128.0 + 224.0 * c) * u_div; }
void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy);
    if (u_plane == 0) {
        o_color = vec4(clamp(word_y(luma(rgbAt(at))), 0.0, 65535.0) / 65535.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 sum = vec3(0.0);
    float n = 0.0;
    for (int j = 0; j < (1 << u_shift.y); j++)
        for (int i = 0; i < (1 << u_shift.x); i++) {
            ivec2 p = ivec2((at.x << u_shift.x) + i, (at.y << u_shift.y) + j);
            if (p.x < u_size.x && p.y < u_size.y) {
                sum += rgbAt(p);
                n += 1.0;
            }
        }
    vec3 c = sum / max(n, 1.0);
    float yy = luma(c);
    float cb = (c.b - yy) / (2.0 * (1.0 - u_k.y));
    float cr = (c.r - yy) / (2.0 * (1.0 - u_k.x));
    float u = clamp(word_c(cb), 0.0, 65535.0) / 65535.0, v = clamp(word_c(cr), 0.0, 65535.0) / 65535.0;
    o_color = u_plane == 1 ? vec4(u, 0.0, 0.0, 1.0) : u_plane == 2 ? vec4(v, 0.0, 0.0, 1.0) : vec4(u, v, 0.0, 1.0);
}
)";

// The luma of a frame as the depth map: the samples as they are (R8), or as the integers they are (R16UI)
const char kLuma[] = R"(
uniform sampler2D u_y;
uniform ivec2 u_origin;
out vec4 o_color;
void main()
{
    o_color = vec4(texelFetch(u_y, ivec2(gl_FragCoord.xy) + u_origin, 0).r, 0.0, 0.0, 1.0);
}
)";
const char kLumaInt[] = R"(
uniform sampler2D u_y;
uniform ivec2 u_origin;
out uvec4 o_color;
void main()
{
    o_color = uvec4(uint(texelFetch(u_y, ivec2(gl_FragCoord.xy) + u_origin, 0).r * 65535.0 + 0.5), 0u, 0u, 1u);
}
)";

// A block of an imported frame's planes at the middle of the picture, as the bytes a check reads back: luma, and the two chroma samples
const char kProbe[] = R"(
uniform sampler2D u_y;
uniform sampler2D u_uv;
uniform ivec2 u_origin;
uniform ivec2 u_size;
out vec4 o_color;
void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy) + u_origin;
    vec2 c = texelFetch(u_uv, clamp(at >> 1, ivec2(0), (u_size >> 1) - 1), 0).rg;
    o_color = vec4(texelFetch(u_y, clamp(at, ivec2(0), u_size - 1), 0).r, c, 1.0);
}
)";

// One channel of an RGBA picture as the depth map
const char kChannel[] = R"(
uniform sampler2D u_y;
uniform int u_channel;
out vec4 o_color;
void main()
{
    o_color = vec4(texelFetch(u_y, ivec2(gl_FragCoord.xy), 0)[u_channel], 0.0, 0.0, 1.0);
}
)";

// The pointers stereo_dibr_unpack() is given for a frame that is not in system memory (it never reads through them: the offsets between
// them place the pictures)
constexpr uintptr_t kImportedBase = uintptr_t(1) << 40;

// A texture of the drawing context: its name, size and internal format
struct GlTex
{
    unsigned id = 0;
    int w = 0, h = 0;
    unsigned internal = 0;
};

class GpuDrawer
{
public:
    // The planes of the drawn views, textures of the drawing context handed to the video sink: RGBA8 for an 8 bit source, the source's own
    // planes (R16, RG16) for a deeper one. generation tells the context they belong to (a new context makes the old ones void).
    struct Views
    {
        std::array<GlTex, 3> plane;
        int nplanes = 0;
        unsigned generation = 0;
        // the views as one RGB picture (RGBA8, or RGBA16 for a deeper source), the two eyes side by side, for the stereo layer's pass: the
        // texture of plane 0 for 8 bit content, a texture of its own (rgbOwned) for a deeper one
        GlTex rgb;
        bool rgbOwned = false;
        // the EGL display and context of the drawing context, when it is one, for a consumer that makes a context sharing it
        void *eglDisplay = nullptr;
        void *eglContext = nullptr;
        bool es = false;
    };

    enum class Source { Planar, SemiUV, SemiVU, Rgba };

    // A source deeper than 8 bits: its words, and what they mean (see kConvert)
    struct Deep
    {
        bool on = false;
        int tableBits = 16;    // the bits of the depth samples' table: the samples are the words
        int precision = 16;    // the bits of the content: the passes' textures never hold less
        float div = 256, max = 65535, mid = 32768;
        int log2w = 0, log2h = 0;
        bool semi = false;
    };

    // The planes of a frame the decoder keeps on a GPU, as textures of the drawing context (QHwVideoBuffer::mapTextures through a
    // QRhi that shares it): the passes read them in place, the frame is never read back to system memory
    struct Imported
    {
        bool on = false;
        unsigned y = 0, uv = 0;
        QVideoFrameTexturesUPtr textures;
    };


    // An OpenGL context belongs to one thread, and a video's frames come from the thread of the
    // decoder, of the item and of the sink in turn: the context lives in a thread of its own, and
    // everything that touches it runs there (run()), the caller waiting
    GpuDrawer()
    {
        m_worker.setObjectName(QStringLiteral("QtMultimedia depth drawing"));
        m_executor.moveToThread(&m_worker);
        m_worker.start();
    }

    ~GpuDrawer()
    {
        run([this] { destroy(); });
        m_worker.quit();
        m_worker.wait();
    }

    template<typename F>
    void run(F function)
    {
        QMetaObject::invokeMethod(&m_executor, std::move(function), Qt::BlockingQueuedConnection);
    }

    // The context and the library's passes, made on the thread that draws (a context belongs to
    // one thread). Returns false with the reason.
    bool ensure(QString *why, QOpenGLContext *share = nullptr)
    {
        if (m_lost) {
            *why = QStringLiteral("the GPU was reset while drawing, it is not used again for this video");
            return false;
        }
        // the sink's context appeared or changed: the drawing context is made again to share its textures
        if (m_gl && m_thread == QThread::currentThread() && (!share || share == m_shareUsed))
            return true;
        if (!m_gl && m_failedThread == QThread::currentThread() && share == m_failedShare) {
            *why = m_whyNot;
            return false;
        }
        destroy();
        m_thread = QThread::currentThread();
        m_failedShare = share;
        m_shareWhy.clear();
        // The context shares the sink's when there is one: a texture of the drawing context is then a texture the sink can draw. It is made
        // of the sink's kind (desktop OpenGL or OpenGL ES); where sharing is refused, the context is made without, and the views are read back.
        std::unique_ptr<QOpenGLContext> context;
        for (int attempt = 0; attempt < (share ? 2 : 1) && !context; ++attempt) {
            QOpenGLContext *with = attempt == 0 ? share : nullptr;
            auto candidate = std::make_unique<QOpenGLContext>();
            if (with)
                candidate->setShareContext(with);
            bool created = false;
            if (!with || !with->isOpenGLES()) {
                QSurfaceFormat format;
                format.setVersion(3, 3);
                format.setProfile(QSurfaceFormat::CoreProfile);
                // a context that tells when the GPU was reset (KHR_robustness): the library watches it (stereo_dibr_gl_reset_status())
                format.setOption(QSurfaceFormat::ResetNotification);
                candidate->setFormat(format);
                created = candidate->create();
            }
            if (!created) {
                // OpenGL ES 3.0, for a platform that has no desktop OpenGL
                if (!with || with->isOpenGLES()) {
                    QSurfaceFormat format;
                    format.setRenderableType(QSurfaceFormat::OpenGLES);
                    format.setVersion(3, 0);
                    format.setOption(QSurfaceFormat::ResetNotification);
                    candidate->setFormat(format);
                    created = candidate->create();
                }
            }
            if (created) {
                m_shared = with && candidate->shareContext() == with;
                m_group = m_shared ? candidate->shareGroup() : nullptr;
                context = std::move(candidate);
            } else if (with) {
                m_shareWhy = QStringLiteral("an OpenGL context sharing the video sink's could not be made");
            }
        }
        if (!context) {
            m_shareUsed = share;
            return fail(QStringLiteral("no OpenGL 3.3 or OpenGL ES 3.0 context could be made"), why);
        }
        m_shareUsed = share;
        auto surface = std::make_unique<QOffscreenSurface>();
        surface->setFormat(context->format());
        surface->create();
        if (!surface->isValid())
            return fail(QStringLiteral("no offscreen surface could be made"), why);
        m_context = std::move(context);
        m_surface = std::move(surface);
        if (!m_context->makeCurrent(m_surface.get()))
            return fail(QStringLiteral("the OpenGL context could not be made current"), why);
        g_depthContext = m_context.get();
        m_eglDisplay = m_eglContext = nullptr;
#if QT_CONFIG(egl)
        if (auto *egl = m_context->nativeInterface<QNativeInterface::QEGLContext>()) {
            m_eglDisplay = egl->display();
            m_eglContext = reinterpret_cast<void *>(egl->nativeContext());
        }
#endif
        m_f = m_context->extraFunctions();
        // 16 bit normalised textures: in the core profile always, on OpenGL ES with GL_EXT_texture_norm16
        m_deepOk = !m_context->isOpenGLES() || m_context->hasExtension(QByteArrayLiteral("GL_EXT_texture_norm16"));
        m_gl = stereo_dibr_gl_create(resolveGl,
                                     m_context->isOpenGLES() ? STEREO_DIBR_GL_ES
                                                             : STEREO_DIBR_GL_DESKTOP);
        if (!m_gl)
            return fail(QStringLiteral("libstereo-dibr's passes do not build on this OpenGL"), why);
        if (!buildPrograms())
            return fail(QStringLiteral("the passes that read a frame's planes do not build"), why);
        m_f->glGenVertexArrays(1, &m_vao);
        m_f->glGenFramebuffers(1, &m_fbo);
        m_context->doneCurrent();
        return true;
    }

    // Whether textures of this context are textures of the sink's (the context shares it), and when not, why
    bool sharesSink(QString *why) const
    {
        if (m_shared)
            return true;
        *why = m_shareWhy.isEmpty() ? QStringLiteral("the drawing context shares no context with the video sink") : m_shareWhy;
        return false;
    }
    // Whether a QRhi draws with OpenGL in the group of this context: the sink can take its textures
    bool takenBy(QRhi &rhi) const
    {
        QOpenGLContextGroup *group = m_group.load();
        if (!group || rhi.backend() != QRhi::OpenGLES2)
            return false;
        const auto *handles = static_cast<const QRhiGles2NativeHandles *>(rhi.nativeHandles());
        return handles && handles->context && handles->context->shareGroup() == group;
    }

    // A frame of textures is let go (from any thread): its textures wait for the next drawing, or are deleted on this context's thread
    void recycle(const Views &v)
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (v.generation != m_generation)
            return;
        for (int i = 0; i < v.nplanes; ++i) {
            m_out.erase(v.plane[i].id);
            (m_pool.size() < 8 ? m_pool : m_trash).push_back(v.plane[i]);
        }
        if (v.rgbOwned) {
            m_out.erase(v.rgb.id);
            (m_pool.size() < 8 ? m_pool : m_trash).push_back(v.rgb);
        }
    }
    void fillEgl(Views *views) const
    {
        views->eglDisplay = m_eglDisplay;
        views->eglContext = m_eglContext;
        views->es = m_context && m_context->isOpenGLES();
    }
    unsigned generation()
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        return m_generation;
    }

    // The planes of a frame of textures read back to system memory (what a consumer that is not the sink asks for), each into dst[i] with
    // its row stride in bytes. Returns false when the context is gone.
    bool readBack(const Views &v, uchar *const *dst, const int *strideBytes)
    {
        bool ok = false;
        run([&] {
            if (v.generation != generation() || !m_context || !makeCurrent())
                return;
            ok = true;
            for (int i = 0; i < v.nplanes && ok; ++i) {
                const GlTex &t = v.plane[i];
                m_f->glBindFramebuffer(GL_FRAMEBUFFER_, m_fbo);
                m_f->glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, t.id, 0);
                ok = m_f->glCheckFramebufferStatus(GL_FRAMEBUFFER_) == GL_FRAMEBUFFER_COMPLETE_;
                if (!ok)
                    break;
                const bool bytes = t.internal == GL_RGBA8_;
                const unsigned format = bytes ? GL_RGBA_ : t.internal == GL_RG16_ ? GL_RG_ : GL_RED_;
                const int pixel = bytes || t.internal == GL_RG16_ ? 4 : 2;
                m_f->glPixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, 1);
                m_f->glPixelStorei(GL_PACK_ROW_LENGTH_, strideBytes[i] / pixel);
                m_f->glReadPixels(0, 0, t.w, t.h, format, bytes ? GL_UNSIGNED_BYTE_ : GL_UNSIGNED_SHORT_, dst[i]);
                m_f->glPixelStorei(GL_PACK_ROW_LENGTH_, 0);
                ok = m_f->glGetError() == 0;
            }
            doneCurrent();
        });
        return ok;
    }

    // The QRhi of the drawing context: the decoder's texture converters map their frames through one
    bool ensureRhi(QString *why)
    {
        if (m_rhi)
            return true;
        if (m_rhiFailed) {
            *why = m_rhiWhy;
            return false;
        }
        QRhiGles2InitParams params;
        params.fallbackSurface = m_surface.get();
        params.format = m_context->format();
        QRhiGles2NativeHandles handles;
        handles.context = m_context.get();
        m_rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, QRhi::Flags(), &handles));
        m_context->makeCurrent(m_surface.get());
        if (!m_rhi) {
            m_rhiFailed = true;
            m_rhiWhy = QStringLiteral("a QRhi could not be made on the drawing context");
            *why = m_rhiWhy;
            return false;
        }
        return true;
    }

    // Whether a frame is one the decoder keeps on a GPU, and when it is (and the person did not ask for a copy), its planes as
    // textures of this context. *hardware: the frame is a hardware frame (its read back is a copy to system memory).
    bool importFrame(QHwVideoBuffer *hw, const QSize &size, int bytes, bool copyAsked, bool *hardware, QString *why)
    {
        *hardware = false;
        m_imp = {};
        if (!ensureRhi(why))
            return false;
        hw->initTextureConverter(*m_rhi);
        if (hw->handleType() != QVideoFrame::RhiTextureHandle)
            return false; // a frame in system memory: nothing to import, nothing said
        *hardware = true;
        if (copyAsked) {
            *why = QStringLiteral("the copy to system memory was asked for");
            return false;
        }
        QVideoFrameTexturesUPtr textures = hw->mapTextures(*m_rhi, m_oldTextures);
        m_context->makeCurrent(m_surface.get());
        if (!textures) {
            *why = QStringLiteral("the decoder's texture converter refused the frame (the DMA-BUF of the decoding GPU cannot be "
                                  "imported by this GPU)");
            return false;
        }
        QRhiTexture *ty = textures->texture(0), *tuv = textures->texture(1);
        if (!ty || !tuv) {
            *why = QStringLiteral("the frame has no two planes (luma and interleaved chroma)");
            return false;
        }
        const QRhiTexture::Format fy = bytes == 2 ? QRhiTexture::R16 : QRhiTexture::R8;
        const QRhiTexture::Format fuv = bytes == 2 ? QRhiTexture::RG16 : QRhiTexture::RG8;
        if (ty->format() != fy || tuv->format() != fuv) {
            *why = QStringLiteral("the planes are textures of a format the passes do not read (%1, %2)")
                           .arg(int(ty->format())).arg(int(tuv->format()));
            return false;
        }
        if ((ty->flags() & QRhiTexture::ExternalOES) || (tuv->flags() & QRhiTexture::ExternalOES)) {
            *why = QStringLiteral("the planes are external (OES) textures");
            return false;
        }
        if (ty->pixelSize() != size) {
            *why = QStringLiteral("the luma texture is %1x%2, the frame %3x%4").arg(ty->pixelSize().width())
                           .arg(ty->pixelSize().height()).arg(size.width()).arg(size.height());
            return false;
        }
        const quint64 gy = ty->nativeTexture().object, guv = tuv->nativeTexture().object;
        if (!gy || !guv) {
            *why = QStringLiteral("the planes have no OpenGL texture");
            return false;
        }
        // The decoder's converter leaves the textures it made of the DMA-BUFs as they were created: a minification filter that needs mipmaps,
        // which makes the texture incomplete and reads as zeros in a pass of ours (Qt's own sampler sets the filter when the scene graph draws
        // them). The passes read level 0 at texel coordinates: the filters are set to nearest.
        for (unsigned id : { unsigned(gy), unsigned(guv) }) {
            m_f->glBindTexture(GL_TEXTURE_2D_, id);
            m_f->glTexParameteri(GL_TEXTURE_2D_, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
            m_f->glTexParameteri(GL_TEXTURE_2D_, 0x2800 /* MAG_FILTER */, 0x2600);
            m_f->glTexParameteri(GL_TEXTURE_2D_, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
            m_f->glTexParameteri(GL_TEXTURE_2D_, 0x2803 /* WRAP_T */, 0x812F);
        }
        // A DMA-BUF the decoding GPU wrote can be accepted by another GPU and read back as zeros (the tiling or the synchronisation of the
        // two devices does not agree). The planes of the first frames, and of every sixtieth, are sampled at the middle of the picture: a
        // block of all-zero luma and chroma is no picture, so the import is refused, said, and the frames are copied from then on.
        if (size != m_checkSize || bytes != m_checkBytes) {
            m_checkSize = size;
            m_checkBytes = bytes;
            m_checked = m_emptyInRow = m_sinceCheck = 0;
            m_importBad = false;
        }
        if (m_importBad) {
            *why = QStringLiteral("the planes the decoder's frames were imported as were empty: this GPU cannot read the decoding GPU's DMA-BUF");
            return false;
        }
        if (m_checked < 8 || ++m_sinceCheck >= 60) {
            m_sinceCheck = 0;
            const int found = sampleImported(unsigned(gy), unsigned(guv), size);
            ++m_checked;
            ++m_checksDone;
            if (found == 1) {
                ++m_checksEmpty;
                if (++m_emptyInRow >= 4 || m_checked > 8) {
                    m_importBad = true;
                    *why = QStringLiteral("the planes the decoder's frames were imported as are empty: this GPU cannot read the decoding GPU's DMA-BUF");
                    return false;
                }
            } else if (found == 0) {
                m_emptyInRow = 0;
            }
        }
        m_imp.y = unsigned(gy);
        m_imp.uv = unsigned(guv);
        m_imp.textures = std::move(textures);
        m_imp.on = true;
        m_importBase = kImportedBase;
        m_importStride = ptrdiff_t(size.width()) * bytes;
        m_importBytes = bytes;
        return true;
    }

    // Whether a block of 32x32 luma samples with their chroma, at the middle of the imported planes, is all zero: 1 for empty, 0 for a picture,
    // -1 when the planes cannot be sampled
    int sampleImported(unsigned y, unsigned uv, const QSize &size)
    {
        const int n = 32;
        makeTexture(ProbeTex, n, n, GL_RGBA8_, GL_RGBA_);
        m_f->glUseProgram(m_probe);
        bindExternal(0, y, m_probe, "u_y");
        bindExternal(1, uv, m_probe, "u_uv");
        m_f->glUniform2i(m_f->glGetUniformLocation(m_probe, "u_origin"), std::max(0, (size.width() - n) / 2 & ~1), std::max(0, (size.height() - n) / 2 & ~1));
        m_f->glUniform2i(m_f->glGetUniformLocation(m_probe, "u_size"), size.width(), size.height());
        pass(m_probe, ProbeTex, n, n);
        uchar block[32 * 32 * 4];
        m_f->glPixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, 1);
        m_f->glReadPixels(0, 0, n, n, GL_RGBA_, GL_UNSIGNED_BYTE_, block);
        if (m_f->glGetError() != 0)
            return -1;
        int most[3] = {};
        for (int i = 0; i < n * n; ++i)
            for (int c = 0; c < 3; ++c)
                most[c] = std::max<int>(most[c], block[4 * i + c]);
        if (most[0] == 0 && most[1] == 0 && most[2] == 0)
            return 1;
        return 0;
    }
    int importChecks() const { return m_checksDone; }
    int importEmpties() const { return m_checksEmpty; }

    // After the drawing: the frame's textures wait for the next one (the converter reuses them) and the frame is let go
    void releaseImport()
    {
        if (m_imp.on)
            m_oldTextures = std::move(m_imp.textures);
        m_imp = {};
    }

    bool makeCurrent()
    {
        g_depthContext = m_context.get();
        return m_context && m_context->makeCurrent(m_surface.get());
    }
    void doneCurrent()
    {
        if (m_context)
            m_context->doneCurrent();
    }
    stereo_dibr_gl *gl() const { return m_gl; }
    // What the driver says is free, with what this drawer holds put back: the memory a way may use, as the controls and the guard of the
    // automatic choice judge it
    double freeMemory() const
    {
        if (!m_gl)
            return -1;
        const double free = stereo_dibr_gl_free_memory(m_gl);
        if (free < 0)
            return -1;
        double held = stereo_dibr_gl_memory_used(m_gl);
        for (const Tex &t : m_tex) {
            const int bpp = t.internal == GL_RGBA16_ ? 8 : (t.internal == GL_RGBA8_ || t.internal == GL_RG16_) ? 4 : (t.internal == GL_RG8_ || t.internal == GL_R16_ || t.internal == GL_R16UI_) ? 2 : 1;
            held += double(t.w) * t.h * bpp;
        }
        return free + held;
    }
    bool lost() const { return m_lost; }
    bool isEs() const { return m_context && m_context->isOpenGLES(); }

    // The views of a picture and its depth map into result (RGBA 8 bit, twice the picture's
    // width), in the current context. parts are the picture and its depth map (stereo_dibr_unpack()
    // of the frame, turned as the frame asks), dparts the same for the depth component of the
    // frame. Returns false with the reason.
    // With views, the drawn views stay on the GPU as textures for the video sink (no read back); without, they are read back into result.
    bool draw(Source source, const Deep &deep, bool fullRange, int colorSpace,
              const stereo_dibr_parts &parts, const stereo_dibr_parts &dparts,
              const stereo_dibr_params &prm, const float *table, int tableSize,
              const float *layerTable, QVideoFrame *result, Views *views, QString *why)
    {
        dropTrash();
        const int vw = parts.picture.width, vh = parts.picture.height;
        if (deep.on && !m_deepOk) {
            *why = QStringLiteral("this OpenGL ES has no GL_EXT_texture_norm16: the 16 bit planes have no texture format");
            return false;
        }
        m_deep = deep;
        // the colour textures of the passes never hold less than the source
        stereo_dibr_gl_set_precision(m_gl, deep.on ? deep.precision : 8);
        while (m_f->glGetError() != 0) { }
        if (!picture(source, fullRange, colorSpace, parts.picture, Pic) ||
            !depth(source, dparts.depth, Depth)) {
            *why = QStringLiteral("a plane could not be uploaded");
            return false;
        }
        const bool layered = parts.layer.width != 0;
        if (layered
            && (!picture(source, fullRange, colorSpace, parts.layer, LayerPic)
                || !depth(source, dparts.layer_depth, LayerDepth))) {
            *why = QStringLiteral("a layer could not be uploaded");
            return false;
        }
        if (deep.on)
            makeTexture(Target, vw * 2, vh, GL_RGBA16_, GL_RGBA_, GL_UNSIGNED_SHORT_);
        else
            makeTexture(Target, vw * 2, vh, GL_RGBA8_, GL_RGBA_);
        const float hole[4] = { 0.f, 0.f, 0.f, 1.f };
        int r;
        if (layered)
            r = stereo_dibr_gl_render_layer(m_gl, &prm, table, tableSize, m_tex[Pic].id, vw, vh,
                                            m_tex[Depth].id, dparts.depth.width,
                                            dparts.depth.height, deep.on ? 1 : 0, parts.layer_role,
                                            m_tex[LayerPic].id, layerTable, tableSize,
                                            m_tex[LayerDepth].id, dparts.layer_depth.width,
                                            dparts.layer_depth.height, m_tex[Target].id, hole);
        else
            r = stereo_dibr_gl_render(m_gl, &prm, table, tableSize, m_tex[Pic].id, vw, vh,
                                      m_tex[Depth].id, dparts.depth.width, dparts.depth.height,
                                      deep.on ? 1 : 0, m_tex[Target].id, hole);
        if (r < 0) {
            if (r == -4 || stereo_dibr_gl_reset_status(m_gl))
                m_lost = true;
            *why = QString::fromLatin1(stereo_dibr_gl_error(m_gl));
            if (why->isEmpty())
                *why = QStringLiteral("the views could not be drawn");
            return false;
        }
        if (deep.on)
            return encodeViews(fullRange, colorSpace, vw, vh, result, views, why);
        if (views) {
            // the target is the frame: handed out as it is, finished before the sink's context draws it
            m_f->glFinish();
            *views = {};
            views->nplanes = 1;
            views->plane[0] = handOut(Target);
            views->rgb = views->plane[0];
            views->generation = m_generation;
            fillEgl(views);
            return m_f->glGetError() == 0 || (*why = QStringLiteral("the views could not be handed to the sink"), false);
        }
        // the views into the frame
        m_f->glBindFramebuffer(GL_FRAMEBUFFER_, m_fbo);
        m_f->glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_,
                                    m_tex[Target].id, 0);
        m_f->glPixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, 1);
        m_f->glPixelStorei(GL_PACK_ROW_LENGTH_, result->bytesPerLine(0) / 4);
        m_f->glReadPixels(0, 0, vw * 2, vh, GL_RGBA_, GL_UNSIGNED_BYTE_, result->bits(0));
        m_f->glPixelStorei(GL_PACK_ROW_LENGTH_, 0);
        if (m_f->glGetError() != 0) {
            *why = QStringLiteral("the views could not be read back");
            return false;
        }
        return true;
    }

private:
    enum TexId { Y, U, V, Raw, Pic, Depth, Target, LayerPic, LayerDepth, OutY, OutU, OutV, ProbeTex, NbTextures };
    using Tex = GlTex;

    bool fail(const QString &why, QString *out)
    {
        m_whyNot = why;
        m_failedThread = m_thread;
        *out = why;
        destroy();
        return false;
    }

    void destroy()
    {
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            ++m_generation;
        }
        if (m_context && m_surface && m_context->makeCurrent(m_surface.get())) {
            g_depthContext = m_context.get();
            if (m_gl)
                stereo_dibr_gl_free(m_gl);
            for (Tex &t : m_tex)
                if (t.id)
                    m_f->glDeleteTextures(1, &t.id);
            {
                // the textures given back, and those still with a frame (GL deletes a texture the sink still uses when it lets go of it)
                std::lock_guard<std::mutex> lock(m_poolMutex);
                for (const GlTex &t : m_pool)
                    m_f->glDeleteTextures(1, &t.id);
                for (const GlTex &t : m_trash)
                    m_f->glDeleteTextures(1, &t.id);
                for (unsigned id : m_out)
                    m_f->glDeleteTextures(1, &id);
                m_pool.clear();
                m_trash.clear();
                m_out.clear();
            }
            if (m_vao)
                m_f->glDeleteVertexArrays(1, &m_vao);
            if (m_fbo)
                m_f->glDeleteFramebuffers(1, &m_fbo);
            if (m_convert)
                m_f->glDeleteProgram(m_convert);
            if (m_channel)
                m_f->glDeleteProgram(m_channel);
            if (m_encode)
                m_f->glDeleteProgram(m_encode);
            if (m_luma)
                m_f->glDeleteProgram(m_luma);
            if (m_lumaInt)
                m_f->glDeleteProgram(m_lumaInt);
            if (m_probe)
                m_f->glDeleteProgram(m_probe);
            m_imp = {};
            m_oldTextures.reset();
            m_rhi.reset();
            m_context->doneCurrent();
        }
        m_gl = nullptr;
        m_tex = {};
        m_vao = m_fbo = m_convert = m_channel = m_encode = m_luma = m_lumaInt = m_probe = 0;
        m_rhiFailed = false;
        m_shared = false;
        m_group = nullptr;
        m_surface.reset();
        m_context.reset();
        m_f = nullptr;
    }

    unsigned compile(unsigned type, const char *body)
    {
        const QByteArray prefix = m_context->isOpenGLES()
                ? QByteArrayLiteral("#version 300 es\nprecision highp float;\nprecision highp int;\n"
                                    "precision highp sampler2D;\n")
                : QByteArrayLiteral("#version 330 core\n");
        const QByteArray source = prefix + body;
        const char *text = source.constData();
        const unsigned shader = m_f->glCreateShader(type);
        m_f->glShaderSource(shader, 1, &text, nullptr);
        m_f->glCompileShader(shader);
        int ok = 0;
        m_f->glGetShaderiv(shader, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
        if (!ok) {
            char log[400] = {};
            m_f->glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            qCWarning(qLcVideoDepth) << "a pass that reads a frame does not compile:" << log;
            m_f->glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    unsigned program(const char *fragment)
    {
        const unsigned vs = compile(0x8B31 /* GL_VERTEX_SHADER */, kVertex);
        const unsigned fs = vs ? compile(0x8B30 /* GL_FRAGMENT_SHADER */, fragment) : 0;
        if (!vs || !fs)
            return 0;
        const unsigned prog = m_f->glCreateProgram();
        m_f->glAttachShader(prog, vs);
        m_f->glAttachShader(prog, fs);
        m_f->glLinkProgram(prog);
        m_f->glDeleteShader(vs);
        m_f->glDeleteShader(fs);
        int ok = 0;
        m_f->glGetProgramiv(prog, 0x8B82 /* GL_LINK_STATUS */, &ok);
        if (!ok) {
            m_f->glDeleteProgram(prog);
            return 0;
        }
        return prog;
    }

    bool buildPrograms()
    {
        m_convert = program(kConvert);
        m_channel = program(kChannel);
        m_encode = program(kEncode);
        m_luma = program(kLuma);
        m_lumaInt = program(kLumaInt);
        m_probe = program(kProbe);
        return m_convert && m_channel && m_encode && m_luma && m_lumaInt && m_probe;
    }

    void makeTexture(TexId id, int w, int h, unsigned internal, unsigned format,
                     unsigned type = GL_UNSIGNED_BYTE_)
    {
        Tex &t = m_tex[id];
        if (!t.id && (id == Target || id == OutY || id == OutU || id == OutV)) {
            // a texture a frame handed out and gave back: the same size and format, drawn into again
            std::lock_guard<std::mutex> lock(m_poolMutex);
            for (auto it = m_pool.begin(); it != m_pool.end(); ++it) {
                if (it->w == w && it->h == h && it->internal == internal) {
                    t = *it;
                    m_pool.erase(it);
                    break;
                }
            }
        }
        if (!t.id)
            m_f->glGenTextures(1, &t.id);
        m_f->glBindTexture(GL_TEXTURE_2D_, t.id);
        if (t.w != w || t.h != h || t.internal != internal) {
            m_f->glTexImage2D(GL_TEXTURE_2D_, 0, int(internal), w, h, 0, format, type, nullptr);
            m_f->glTexParameteri(GL_TEXTURE_2D_, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
            m_f->glTexParameteri(GL_TEXTURE_2D_, 0x2800 /* MAG_FILTER */, 0x2600);
            m_f->glTexParameteri(GL_TEXTURE_2D_, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
            m_f->glTexParameteri(GL_TEXTURE_2D_, 0x2803 /* WRAP_T */, 0x812F);
            t.w = w;
            t.h = h;
            t.internal = internal;
        }
    }

    // A plane of w x h samples of bytes each, row to row stride bytes apart, into texture id
    bool upload(TexId id, int w, int h, int bytes, const void *data, ptrdiff_t stride)
    {
        if (!data || stride <= 0 || w <= 0 || h <= 0)
            return false;
        const unsigned internal = bytes == 1 ? GL_R8_ : bytes == 2 ? GL_RG8_ : GL_RGBA8_;
        const unsigned format = bytes == 1 ? GL_RED_ : bytes == 2 ? GL_RG_ : GL_RGBA_;
        makeTexture(id, w, h, internal, format);
        m_f->glPixelStorei(0x0CF5 /* GL_UNPACK_ALIGNMENT */, 1);
        if (stride % bytes == 0) {
            m_f->glPixelStorei(GL_UNPACK_ROW_LENGTH_, int(stride / bytes));
            m_f->glTexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE_, data);
            m_f->glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
        } else {
            std::vector<uint8_t> rows(size_t(w) * bytes * h);
            for (int y = 0; y < h; ++y)
                std::memcpy(rows.data() + size_t(y) * w * bytes,
                            static_cast<const uint8_t *>(data) + y * stride, size_t(w) * bytes);
            m_f->glTexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE_,
                                 rows.data());
        }
        return true;
    }

    // A plane of 16 bit samples (1 or 2 channels), the words as they are: R16 / RG16, or R16UI for the depth samples
    bool uploadWords(TexId id, int w, int h, int channels, bool integer, const void *data,
                     ptrdiff_t stride)
    {
        const int bpp = 2 * channels;
        if (!data || stride <= 0 || w <= 0 || h <= 0)
            return false;
        const unsigned internal = integer ? GL_R16UI_ : channels == 1 ? GL_R16_ : GL_RG16_;
        const unsigned format = integer ? GL_RED_INTEGER_ : channels == 1 ? GL_RED_ : GL_RG_;
        makeTexture(id, w, h, internal, format, GL_UNSIGNED_SHORT_);
        m_f->glPixelStorei(0x0CF5 /* GL_UNPACK_ALIGNMENT */, 1);
        if (stride % bpp == 0) {
            m_f->glPixelStorei(GL_UNPACK_ROW_LENGTH_, int(stride / bpp));
            m_f->glTexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, w, h, format, GL_UNSIGNED_SHORT_, data);
            m_f->glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
        } else {
            std::vector<uint8_t> rows(size_t(w) * bpp * h);
            for (int y = 0; y < h; ++y)
                std::memcpy(rows.data() + size_t(y) * w * bpp,
                            static_cast<const uint8_t *>(data) + y * stride, size_t(w) * bpp);
            m_f->glTexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, w, h, format, GL_UNSIGNED_SHORT_, rows.data());
        }
        return true;
    }

    // A plane of the output at the source's depth, read back into the frame's plane
    bool readPlane(TexId id, int w, int h, int channels, uchar *dst, int strideBytes)
    {
        m_f->glBindFramebuffer(GL_FRAMEBUFFER_, m_fbo);
        m_f->glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, m_tex[id].id, 0);
        if (m_f->glCheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_)
            return false;
        m_f->glPixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, 1);
        m_f->glPixelStorei(GL_PACK_ROW_LENGTH_, strideBytes / (2 * channels));
        m_f->glReadPixels(0, 0, w, h, channels == 1 ? GL_RED_ : GL_RG_, GL_UNSIGNED_SHORT_, dst);
        m_f->glPixelStorei(GL_PACK_ROW_LENGTH_, 0);
        return m_f->glGetError() == 0;
    }

    void kForMatrix(int colorSpace, float *kr, float *kb)
    {
        *kr = 0.299f;
        *kb = 0.114f;
        if (colorSpace == 1) {
            *kr = 0.2126f;
            *kb = 0.0722f;
        } else if (colorSpace == 2) {
            *kr = 0.2627f;
            *kb = 0.0593f;
        }
    }

    // The planes of the views at the source's depth, into the frame (result, the source's pixel format)
    bool encodeViews(bool fullRange, int colorSpace, int vw, int vh, QVideoFrame *result, Views *views, QString *why)
    {
        float kr, kb;
        kForMatrix(colorSpace, &kr, &kb);
        const int tw = vw * 2;
        auto run = [&](int plane, TexId out, int w, int h, int channels) {
            makeTexture(out, w, h, channels == 1 ? GL_R16_ : GL_RG16_, channels == 1 ? GL_RED_ : GL_RG_,
                        GL_UNSIGNED_SHORT_);
            m_f->glUseProgram(m_encode);
            bindTexture(0, Target, m_encode, "u_target");
            auto u = [&](const char *n) { return m_f->glGetUniformLocation(m_encode, n); };
            m_f->glUniform1i(u("u_plane"), plane);
            m_f->glUniform2i(u("u_shift"), m_deep.log2w, m_deep.log2h);
            m_f->glUniform2i(u("u_size"), tw, vh);
            m_f->glUniform2f(u("u_k"), kr, kb);
            m_f->glUniform1i(u("u_full"), fullRange ? 1 : 0);
            m_f->glUniform1f(u("u_div"), m_deep.div);
            m_f->glUniform1f(u("u_max"), m_deep.max);
            m_f->glUniform1f(u("u_mid"), m_deep.mid);
            pass(m_encode, out, w, h);
        };
        run(0, OutY, tw, vh, 1);
        if (views)
            *views = {};
        if (!views && !readPlane(OutY, tw, vh, 1, result->bits(0), result->bytesPerLine(0))) {
            *why = QStringLiteral("the luma of the views could not be read back");
            return false;
        }
        const int cw = (tw + (1 << m_deep.log2w) - 1) >> m_deep.log2w;
        const int ch = (vh + (1 << m_deep.log2h) - 1) >> m_deep.log2h;
        if (m_deep.semi) {
            run(3, OutU, cw, ch, 2);
            if (!views && !readPlane(OutU, cw, ch, 2, result->bits(1), result->bytesPerLine(1))) {
                *why = QStringLiteral("the chroma of the views could not be read back");
                return false;
            }
        } else {
            run(1, OutU, cw, ch, 1);
            run(2, OutV, cw, ch, 1);
            if (!views
                && (!readPlane(OutU, cw, ch, 1, result->bits(1), result->bytesPerLine(1))
                    || !readPlane(OutV, cw, ch, 1, result->bits(2), result->bytesPerLine(2)))) {
                *why = QStringLiteral("the chroma of the views could not be read back");
                return false;
            }
        }
        if (views) {
            // the planes are the frame: handed out as they are, finished before the sink's context draws them
            m_f->glFinish();
            views->rgb = handOut(Target);
            views->rgbOwned = true;
            views->plane[0] = handOut(OutY);
            views->plane[1] = handOut(OutU);
            views->nplanes = 2;
            if (!m_deep.semi) {
                views->plane[2] = handOut(OutV);
                views->nplanes = 3;
            }
            views->generation = m_generation;
            fillEgl(views);
            if (m_f->glGetError() != 0) {
                *why = QStringLiteral("the views could not be handed to the sink");
                return false;
            }
        }
        return true;
    }

    // The texture of a slot leaves the drawer with a frame: the slot is made again (or taken from the textures given back) for the next
    GlTex handOut(TexId id)
    {
        GlTex t = m_tex[id];
        m_tex[id] = {};
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_out.insert(t.id);
        return t;
    }

    void dropTrash()
    {
        std::vector<GlTex> trash;
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            trash.swap(m_trash);
        }
        for (const GlTex &t : trash)
            m_f->glDeleteTextures(1, &t.id);
    }

    void pass(unsigned prog, TexId out, int w, int h)
    {
        m_f->glBindFramebuffer(GL_FRAMEBUFFER_, m_fbo);
        m_f->glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_,
                                    m_tex[out].id, 0);
        m_f->glViewport(0, 0, w, h);
        m_f->glDisable(0x0BE2 /* BLEND */);
        m_f->glDisable(0x0C11 /* SCISSOR_TEST */);
        m_f->glUseProgram(prog);
        m_f->glBindVertexArray(m_vao);
        m_f->glDrawArrays(0x0004 /* TRIANGLES */, 0, 3);
    }

    void bindTexture(int unit, TexId id, unsigned prog, const char *name)
    {
        m_f->glActiveTexture(0x84C0 + unit);
        m_f->glBindTexture(GL_TEXTURE_2D_, m_tex[id].id);
        m_f->glUniform1i(m_f->glGetUniformLocation(prog, name), unit);
    }

    // Where a picture of the frame starts in its planes, from the pointers stereo_dibr_unpack() made of the frame's layout
    bool originOf(const void *p, int *x, int *y) const
    {
        const uintptr_t off = uintptr_t(p) - m_importBase;
        const ptrdiff_t stride = m_importStride, bytes = m_importBytes;
        if (stride <= 0 || bytes <= 0 || uintptr_t(p) < m_importBase)
            return false;
        *y = int(off / uintptr_t(stride));
        *x = int((off % uintptr_t(stride)) / uintptr_t(bytes));
        return true;
    }

    void bindExternal(int unit, unsigned id, unsigned prog, const char *name)
    {
        m_f->glActiveTexture(0x84C0 + unit);
        m_f->glBindTexture(GL_TEXTURE_2D_, id);
        m_f->glUniform1i(m_f->glGetUniformLocation(prog, name), unit);
    }

    // One picture (or layer) of a frame into the RGBA texture out
    bool picture(Source source, bool fullRange, int colorSpace, const stereo_dibr_picture &pic,
                 TexId out)
    {
        const int w = pic.width, h = pic.height;
        if (source == Source::Rgba) // stored as RGBA: the texture is the picture
            return upload(out, w, h, 4, pic.plane[0].data, pic.plane[0].stride);
        const stereo_dibr_plane &py = pic.plane[0], &pu = pic.plane[1];
        const int cw = (w + (1 << pu.log2_w) - 1) >> pu.log2_w;
        const int ch = (h + (1 << pu.log2_h) - 1) >> pu.log2_h;
        int ox = 0, oy = 0;
        if (m_imp.on) {
            // the decoder's planes, read where they are: the picture's place in them is the offset of its pointer
            if (!originOf(py.data, &ox, &oy) || ((ox | oy) & 1))
                return false;
        } else if (m_deep.on) {
            if (!uploadWords(Y, w, h, 1, false, py.data, py.stride))
                return false;
            if (source == Source::Planar) {
                const stereo_dibr_plane &pv = pic.plane[2];
                if (!uploadWords(U, cw, ch, 1, false, pu.data, pu.stride)
                    || !uploadWords(V, cw, ch, 1, false, pv.data, pv.stride))
                    return false;
            } else if (!uploadWords(U, cw, ch, 2, false, pu.data, pu.stride)) {
                return false;
            }
        } else {
            if (!upload(Y, w, h, 1, py.data, py.stride))
                return false;
            if (source == Source::Planar) {
                const stereo_dibr_plane &pv = pic.plane[2];
                if (!upload(U, cw, ch, 1, pu.data, pu.stride) || !upload(V, cw, ch, 1, pv.data, pv.stride))
                    return false;
            } else if (!upload(U, cw, ch, 2, pu.data, pu.stride)) {
                return false;
            }
        }
        if (m_deep.on)
            makeTexture(out, w, h, GL_RGBA16_, GL_RGBA_, GL_UNSIGNED_SHORT_);
        else
            makeTexture(out, w, h, GL_RGBA8_, GL_RGBA_);
        m_f->glUseProgram(m_convert);
        if (m_imp.on) {
            bindExternal(0, m_imp.y, m_convert, "u_y");
            bindExternal(1, m_imp.uv, m_convert, "u_u");
            bindExternal(2, m_imp.uv, m_convert, "u_v");
        } else {
            bindTexture(0, Y, m_convert, "u_y");
            bindTexture(1, U, m_convert, "u_u");
            bindTexture(2, source == Source::Planar ? V : U, m_convert, "u_v");
        }
        m_f->glUniform2i(m_f->glGetUniformLocation(m_convert, "u_origin"), ox, oy);
        m_f->glUniform1i(m_f->glGetUniformLocation(m_convert, "u_mode"),
                         source == Source::Planar ? 0 : source == Source::SemiUV ? 1 : 2);
        m_f->glUniform2i(m_f->glGetUniformLocation(m_convert, "u_shift"), pu.log2_w, pu.log2_h);
        float kr = 0.299f, kb = 0.114f;
        if (colorSpace == 1) {
            kr = 0.2126f;
            kb = 0.0722f;
        } else if (colorSpace == 2) {
            kr = 0.2627f;
            kb = 0.0593f;
        }
        m_f->glUniform2f(m_f->glGetUniformLocation(m_convert, "u_k"), kr, kb);
        m_f->glUniform1i(m_f->glGetUniformLocation(m_convert, "u_full"), fullRange ? 1 : 0);
        m_f->glUniform1f(m_f->glGetUniformLocation(m_convert, "u_scale"), m_deep.on ? 65535.f : 255.f);
        m_f->glUniform1f(m_f->glGetUniformLocation(m_convert, "u_div"), m_deep.on ? m_deep.div : 1.f);
        m_f->glUniform1f(m_f->glGetUniformLocation(m_convert, "u_max"), m_deep.on ? m_deep.max : 255.f);
        m_f->glUniform1f(m_f->glGetUniformLocation(m_convert, "u_mid"), m_deep.on ? m_deep.mid : 128.f);
        pass(m_convert, out, w, h);
        m_f->glUniform2i(m_f->glGetUniformLocation(m_convert, "u_origin"), 0, 0);
        return true;
    }

    // The depth map of a picture: the luma plane as it is, or one channel of an RGBA picture
    bool depth(Source source, const stereo_dibr_depth &d, TexId out)
    {
        if (d.bytes == 4 && source == Source::Rgba) {
            if (!upload(Raw, d.width, d.height, 4, d.data, d.stride))
                return false;
            makeTexture(out, d.width, d.height, GL_R8_, GL_RED_);
            m_f->glUseProgram(m_channel);
            bindTexture(0, Raw, m_channel, "u_y");
            m_f->glUniform1i(m_f->glGetUniformLocation(m_channel, "u_channel"), 1);
            pass(m_channel, out, d.width, d.height);
            return true;
        }
        if (m_imp.on) {
            // the luma of the decoder's frame, where it is, into the texture the passes read
            int ox = 0, oy = 0;
            if (!originOf(d.data, &ox, &oy))
                return false;
            if (m_deep.on)
                makeTexture(out, d.width, d.height, GL_R16UI_, GL_RED_INTEGER_, GL_UNSIGNED_SHORT_);
            else
                makeTexture(out, d.width, d.height, GL_R8_, GL_RED_);
            const unsigned prog = m_deep.on ? m_lumaInt : m_luma;
            m_f->glUseProgram(prog);
            bindExternal(0, m_imp.y, prog, "u_y");
            m_f->glUniform2i(m_f->glGetUniformLocation(prog, "u_origin"), ox, oy);
            pass(prog, out, d.width, d.height);
            return true;
        }
        if (m_deep.on)
            return d.bytes == 2 && uploadWords(out, d.width, d.height, 1, true, d.data, d.stride);
        if (d.bytes != 1)
            return false;
        return upload(out, d.width, d.height, 1, d.data, d.stride);
    }

    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOffscreenSurface> m_surface;
    QOpenGLExtraFunctions *m_f = nullptr;
    QThread *m_thread = nullptr, *m_failedThread = nullptr;
    QString m_whyNot;
    stereo_dibr_gl *m_gl = nullptr;
    QThread m_worker;
    QObject m_executor;
    std::array<Tex, NbTextures> m_tex;
    unsigned m_vao = 0, m_fbo = 0, m_convert = 0, m_channel = 0, m_encode = 0;
    bool m_lost = false;
    // the context the drawing context shares (the sink's), whether it does, and what the pool of textures handed out holds
    QOpenGLContext *m_shareUsed = nullptr, *m_failedShare = nullptr;
    bool m_shared = false;
    std::atomic<QOpenGLContextGroup *> m_group { nullptr };
    QString m_shareWhy;
    std::mutex m_poolMutex;
    std::vector<GlTex> m_pool, m_trash;
    std::set<unsigned> m_out;
    unsigned m_generation = 1;
    bool m_deepOk = true;
    void *m_eglDisplay = nullptr, *m_eglContext = nullptr;
    Deep m_deep;
    std::unique_ptr<QRhi> m_rhi;
    bool m_rhiFailed = false;
    QString m_rhiWhy;
    Imported m_imp;
    QVideoFrameTexturesUPtr m_oldTextures;
    unsigned m_luma = 0, m_lumaInt = 0, m_probe = 0;
    // what the checks of imported planes found: the frame size and format they are for, the checks made, the empty ones in a row, and the verdict
    QSize m_checkSize;
    int m_checkBytes = 0;
    int m_checked = 0, m_emptyInRow = 0, m_sinceCheck = 0;
    bool m_importBad = false;
    int m_checksDone = 0, m_checksEmpty = 0;
    uintptr_t m_importBase = 0;
    ptrdiff_t m_importStride = 0, m_importBytes = 1;
};

// The drawn views as a frame of textures of the drawing context (handed to the sink as they are) and, for a consumer that maps the frame, a
// read back of those textures (counted)
class DrawnViewsBuffer final : public QHwVideoBuffer
{
public:
    // The frame does not keep the drawer alive: the drawer (its context, its QRhi and the decoder's converters made on it) goes with the
    // Renderer, before the decoder's own state that the converters use goes (a CUDA converter pushes the decoder's CUDA context when it is
    // let go). A frame that outlives it has no texture to give and nothing to read back.
    DrawnViewsBuffer(const std::shared_ptr<GpuDrawer> &drawer, const GpuDrawer::Views &views, const QVideoFrameFormat &format,
                     std::shared_ptr<std::atomic<int>> readBacks)
        : QHwVideoBuffer(QVideoFrame::RhiTextureHandle), m_drawer(drawer), m_views(views), m_format(format),
          m_readBacks(std::move(readBacks))
    {
    }
    ~DrawnViewsBuffer() override
    {
        if (const auto drawer = m_drawer.lock())
            drawer->recycle(m_views);
    }

    MapData map(QVideoFrame::MapMode mode) override
    {
        if (mode != QVideoFrame::ReadOnly)
            return {};
        if (!m_mapped) {
            uchar *dst[3] = {};
            int stride[3] = {};
            for (int i = 0; i < m_views.nplanes; ++i) {
                const GlTex &t = m_views.plane[i];
                const int pixel = t.internal == GL_RGBA8_ || t.internal == GL_RG16_ ? 4 : 2;
                stride[i] = t.w * pixel;
                m_bytes[i].resize(qsizetype(stride[i]) * t.h);
                dst[i] = reinterpret_cast<uchar *>(m_bytes[i].data());
                m_stride[i] = stride[i];
            }
            const auto drawer = m_drawer.lock();
            if (!drawer || !drawer->readBack(m_views, dst, stride))
                return {};
            ++*m_readBacks;
            m_mapped = true;
        }
        MapData data;
        data.planeCount = m_views.nplanes;
        for (int i = 0; i < m_views.nplanes; ++i) {
            data.bytesPerLine[i] = m_stride[i];
            data.data[i] = reinterpret_cast<uchar *>(m_bytes[i].data());
            data.dataSize[i] = int(m_bytes[i].size());
        }
        return data;
    }
    void unmap() override { }
    QVideoFrameFormat format() const override { return m_format; }
    // What the stereo layer needs to draw these views itself, in a context of its own that shares the drawing context's textures
    bool layerSource(LayerSource *out) const
    {
        const auto drawer = m_drawer.lock();
        if (!drawer || m_views.generation != drawer->generation() || !m_views.rgb.id || !m_views.eglDisplay || !m_views.eglContext)
            return false;
        out->eglDisplay = m_views.eglDisplay;
        out->eglContext = m_views.eglContext;
        out->es = m_views.es;
        out->texture = m_views.rgb.id;
        out->width = m_views.rgb.w;
        out->height = m_views.rgb.h;
        out->deep = m_views.rgb.internal == GL_RGBA16_;
        return true;
    }
    quint64 textureHandle(QRhi &rhi, int plane) override
    {
        const auto drawer = m_drawer.lock();
        if (!drawer || plane < 0 || plane >= m_views.nplanes || m_views.generation != drawer->generation() || !drawer->takenBy(rhi))
            return 0;
        return m_views.plane[plane].id;
    }

private:
    std::weak_ptr<GpuDrawer> m_drawer;
    GpuDrawer::Views m_views;
    QVideoFrameFormat m_format;
    std::shared_ptr<std::atomic<int>> m_readBacks;
    std::array<QByteArray, 3> m_bytes;
    int m_stride[3] = {};
    bool m_mapped = false;
};

#endif // QT_MM_DEPTH_GPU

} // namespace

bool layerSourceOf(const QVideoFrame &frame, LayerSource *out)
{
#ifdef QT_MM_DEPTH_GPU
    QHwVideoBuffer *hw = QVideoFramePrivate::hwBuffer(frame);
    const auto *drawn = dynamic_cast<const DrawnViewsBuffer *>(hw);
    return drawn && drawn->layerSource(out);
#else
    Q_UNUSED(frame);
    Q_UNUSED(out);
    return false;
#endif
}

struct Renderer::Private
{
    enum class Outcome { Drawn, PictureAlone, Failed, GpuFailed };

    stereo_dibr *dibr = nullptr;
    std::vector<float> table, layerTable;
    std::vector<uint16_t> lut;
#ifdef QT_MM_DEPTH_GPU
    // owned here; the frames of textures handed to the sink look at it without keeping it alive
    std::shared_ptr<GpuDrawer> gpuOwner = std::make_shared<GpuDrawer>();
    GpuDrawer &gpu = *gpuOwner;
#endif
    // the video sink (its RHI), and how the last drawn views reached it
    QRhi *sink = nullptr;
    ViewsRoute viewsRoute = ViewsRoute::Memory;
    QString viewsWhy;
    std::shared_ptr<std::atomic<int>> viewReadBacks = std::make_shared<std::atomic<int>>(0);
    QString saidViews;
    // what the automatic way chose for the last frame, and where it was drawn
    Choice choice;
    bool onGpu = false;
    // the route of the last frame the decoder keeps on a GPU, and what it cost
    FrameRoute route = FrameRoute::Memory;
    QString routeWhy;
    int hwReadBacks = 0;
    bool importLive = false;
    GpuMemory gpuMem;
    int memoryFrames = 0;
    // the times of the ways on the engine that draws, for the size drawn
    struct Timing
    {
        bool ok = false;
        bool gpu = false;
        QString key;
        int w = 0, h = 0;
        double fps = 0;
        std::vector<double> ms;
    } timing;
    // what the log was told last, so that it says it when it changes and not for every frame
    QString said;
    QString saidRoute;

    void sayRoute(const QString &where)
    {
        if (saidRoute == where)
            return;
        saidRoute = where;
        qCInfo(qLcVideoDepth).noquote() << where;
    }

    void sayViews(const QString &where)
    {
        if (saidViews == where)
            return;
        saidViews = where;
        qCInfo(qLcVideoDepth).noquote() << where;
    }

    ~Private() { stereo_dibr_free(dibr); }

    void say(const QString &where, bool warning)
    {
        if (said == where)
            return;
        said = where;
        if (warning)
            qCWarning(qLcVideoDepth).noquote() << where;
        else
            qCInfo(qLcVideoDepth).noquote() << where;
    }

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

    // The way to draw with: the person's, or the first way of libstereo-dibr's ladder that keeps
    // up with the video's frame rate on the engine that draws, from its times at this size (timed
    // once for each GPU and size and kept in the person's cache). The engine's context is current
    // for the GPU. Returns false when the ways cannot be timed.
    bool chooseWay(const Options &o, bool gpuEngine, int w, int h, double fps, int *warp, int *fill)
    {
        if (!o.automatic) {
            choice = {};
            *warp = o.warp;
            *fill = o.fill;
            return true;
        }
        const int n = stereo_dibr_ways(nullptr, 0);
        QString key = QStringLiteral("cpu");
#ifdef QT_MM_DEPTH_GPU
        if (gpuEngine)
            key = QString::fromLatin1(stereo_dibr_gl_gpu(gpu.gl()));
#endif
        // the ladder is timed from the cheapest way up for this frame rate, and goes on when a lower one leaves room
        if (!timing.ok || timing.gpu != gpuEngine || timing.key != key || timing.w != w
            || timing.h != h || std::abs(timing.fps - fps) > 0.01) {
            std::vector<double> ms(size_t(n), -1.0);
            int measured = 0, r = -1;
#ifdef QT_MM_DEPTH_GPU
            if (gpuEngine)
                r = stereo_dibr_gl_measure(gpu.gl(), w, h, 1, fps, ms.data(), n, &measured);
#endif
            if (!gpuEngine)
                r = stereo_dibr_measure_cpu(w, h, fps, ms.data(), n, &measured);
            if (r < 0)
                return false;
            timing = { true, gpuEngine, key, w, h, fps, ms };
            QStringList times;
            for (double t : ms)
                times << (t == -2.0 ? QStringLiteral("no-memory") : t < 0 ? QStringLiteral("not-timed")
                                                                            : QString::number(t, 'f', 1));
            qCInfo(qLcVideoDepth).noquote()
                    << (measured ? QStringLiteral("timed") : QStringLiteral("took from the cache"))
                    << "the ways of drawing the views of a depth video on" << key << "at" << w << "x"
                    << h << "(ms a frame, best way first):" << times.join(QStringLiteral(" "));
        }
        stereo_dibr_choice c;
        if (stereo_dibr_choose_from_times(timing.ms.data(), n, fps, &c) < 0)
            return false;
        choice.valid = true;
        choice.warp = c.warp;
        choice.fill = c.fill;
        choice.keepsUp = c.keeps_up != 0;
        choice.fps = fps;
        choice.ms = c.ms;
        choice.budgetMs = c.budget_ms;
        choice.skippedWarp = c.skipped_warp;
        choice.skippedFill = c.skipped_fill;
        choice.skippedMs = c.skipped_ms;
        choice.onGpu = gpuEngine;
        choice.gpu = key;
        *warp = c.warp;
        *fill = c.fill;
        return true;
    }

    Outcome render(const QVideoFrame &input, Mode mode, bool depthFirst, const QString &fileMark,
                   const Options &options, bool useGpu, QVideoFrame *out, QString *why);
};

namespace {

// The pixel formats the GPU reads as they are: 8 bit planar and semi-planar YUV, and RGBA as
// stored (the frames of hardware decoders and of other formats arrive as RGBA 8 bit images)
#ifdef QT_MM_DEPTH_GPU
bool gpuSourceOf(PixelFormat format, const Layout &l, GpuDrawer::Source *source, GpuDrawer::Deep *deep)
{
    *deep = {};
    if (l.bits > 8) {
        // 10, 12 and 16 bit samples as the words they are: P010 and P016 hold the code in the high bits (the 16 bit word is
        // the 8 bit code times 256 and more), YUV420P10 in the low bits
        deep->on = true;
        deep->tableBits = l.bits;
        deep->log2w = l.c[1].log2w;
        deep->log2h = l.c[1].log2h;
        deep->semi = l.semiPlanar;
        switch (format) {
        case PixelFormat::Format_YUV420P10:
            deep->precision = 10;
            deep->div = 4;
            deep->max = 1023;
            deep->mid = 512;
            break;
        case PixelFormat::Format_P010:
            deep->precision = 10;
            break;
        case PixelFormat::Format_P016:
        case PixelFormat::Format_YUV422P16:
        case PixelFormat::Format_YUV444P16:
            break;
        default:
            return false;
        }
        *source = l.semiPlanar ? GpuDrawer::Source::SemiUV : GpuDrawer::Source::Planar;
        return true;
    }
    switch (format) {
    case PixelFormat::Format_YUV420P:
    case PixelFormat::Format_YV12:
    case PixelFormat::Format_YUV422P:
    case PixelFormat::Format_YUV444P:
        *source = GpuDrawer::Source::Planar;
        return true;
    case PixelFormat::Format_NV12:
        *source = GpuDrawer::Source::SemiUV;
        return true;
    case PixelFormat::Format_NV21:
        *source = GpuDrawer::Source::SemiVU;
        return true;
    case PixelFormat::Format_RGBA8888:
        *source = GpuDrawer::Source::Rgba;
        return true;
    default:
        return false;
    }
}
#endif

// The frame rate of a video: what the stream says, else what its frames' times say, else 30 (said)
double frameRateOf(const QVideoFrame &frame, bool *known)
{
    double fps = frame.surfaceFormat().streamFrameRate();
    if (fps <= 0.0 && frame.endTime() > frame.startTime())
        fps = 1e6 / double(frame.endTime() - frame.startTime());
    *known = fps > 0.0 && fps < 1000.0;
    return *known ? fps : 30.0;
}

} // namespace

Renderer::Private::Outcome Renderer::Private::render(const QVideoFrame &input, Mode mode,
                                                     bool depthFirst, const QString &fileMark,
                                                     const Options &options, bool useGpu,
                                                     QVideoFrame *out, QString *why)
{
    const int packing = packingOf(mode);
    QVideoFrame frame = input;
    Layout l;
    if (!layoutOf(frame.pixelFormat(), &l)) {
        // packed YUV and the formats of hardware textures: drawn from their 8 bit RGBA image
        // as stored: the transformation turns the parts below, not the packed frame
        const QImage image =
                qImageFromVideoFrame(frame, VideoTransformation{}, QVideoFrameImageDepth::Bits8);
        if (image.isNull())
            return Outcome::Failed;
        frame = QVideoFrame(image.convertToFormat(QImage::Format_RGBA8888));
        if (!layoutOf(frame.pixelFormat(), &l))
            return Outcome::Failed;
    }
    bool deepGpu = false; // the GPU draws a source of more than 8 bits at its depth
#ifdef QT_MM_DEPTH_GPU
    GpuDrawer::Source source = GpuDrawer::Source::Planar;
    GpuDrawer::Deep deep;
    if (useGpu && !gpuSourceOf(frame.pixelFormat(), l, &source, &deep)) {
        *why = QStringLiteral("the pixel format %1 is not one the GPU reads")
                       .arg(QVideoFrameFormat::pixelFormatToString(frame.pixelFormat()));
        return Outcome::GpuFailed;
    }
    deepGpu = useGpu && deep.on;
#else
    Q_UNUSED(why);
    if (useGpu)
        return Outcome::GpuFailed;
#endif
    // A frame the decoder keeps on a GPU (VA-API) is read where it is, by the passes of the drawing context, when the decoder's
    // texture converter can map it there: no read back to system memory. Otherwise it is copied, and the log says why.
    bool imported = false;
    route = FrameRoute::Memory;
    routeWhy.clear();
#ifdef QT_MM_DEPTH_GPU
    if (importLive) {
        // a frame imported earlier and not drawn (the draw stopped before the passes): let it go
        gpu.run([&] { gpu.makeCurrent(); gpu.releaseImport(); gpu.doneCurrent(); });
        importLive = false;
    }
    if (useGpu) {
        if (QHwVideoBuffer *hw = QVideoFramePrivate::hwBuffer(input)) {
            const VideoTransformation turn = qNormalizedFrameTransformation(input);
            bool hardware = false;
            QString reason;
            bool ok = false;
            if (turn.rotation != QtVideo::Rotation::None || turn.mirroredHorizontallyAfterRotation) {
                // the pictures of a turned frame are turned on the processor's copy
                bool dummy = false;
                gpu.run([&] { ok = gpu.makeCurrent() && gpu.importFrame(hw, QSize(), 1, true, &dummy, &reason); gpu.doneCurrent(); });
                hardware = dummy;
                reason = QStringLiteral("the frame is turned (rotated or mirrored)");
            } else if (!l.semiPlanar || source != GpuDrawer::Source::SemiUV) {
                bool dummy = false;
                gpu.run([&] { ok = gpu.makeCurrent() && gpu.importFrame(hw, QSize(), 1, true, &dummy, &reason); gpu.doneCurrent(); });
                hardware = dummy;
                reason = QStringLiteral("the frame's planes are not luma and interleaved chroma");
            } else {
                gpu.run([&] {
                    ok = gpu.makeCurrent()
                            && gpu.importFrame(hw, frame.size(), l.bytes, !options.importFrames, &hardware, &reason);
                    gpu.doneCurrent();
                });
            }
            if (ok) {
                imported = true;
                importLive = true;
                route = FrameRoute::Imported;
                sayRoute(QStringLiteral("hardware frames of the decoder are read by the drawing passes where they are, no read back to "
                                        "system memory (mapped through the decoder's texture converter)"));
            } else if (hardware) {
                route = FrameRoute::Copied;
                routeWhy = reason;
                sayRoute(QStringLiteral("hardware frames of the decoder are copied to system memory: %1").arg(reason));
            }
        }
    }
#endif
    auto unmapFrame = [&] {
        if (!imported)
            frame.unmap();
    };
    if (!imported) {
        // the copy to system memory: a hardware frame is read back by the decoder's map()
        QHwVideoBuffer *hw = QVideoFramePrivate::hwBuffer(input);
        if (hw && hw->handleType() == QVideoFrame::RhiTextureHandle) {
            if (route != FrameRoute::Copied) {
                route = FrameRoute::Copied;
                routeWhy = useGpu ? QStringLiteral("the frame was not imported") : QStringLiteral("the views are drawn on the processor");
            }
            ++hwReadBacks;
        }
        if (!frame.map(QVideoFrame::ReadOnly))
            return Outcome::Failed;
    }

    const int w = frame.width(), h = frame.height();
    const bool fullRange = frame.surfaceFormat().colorRange() == QVideoFrameFormat::ColorRange_Full
            || l.c[0].kind == Component::Color;
    uchar *inBits[4] = {};
    int inStrides[4] = {};
    if (imported) {
        // no pointers into memory: the layout's offsets, for stereo_dibr_unpack() to place the pictures
        for (int p = 0; p < (l.semiPlanar ? 2 : 3); ++p) {
            inBits[p] = reinterpret_cast<uchar *>(kImportedBase + (uintptr_t(p) << 36));
            inStrides[p] = w * l.bytes;
        }
    } else {
        for (int p = 0; p < frame.planeCount() && p < 4; ++p) {
            inBits[p] = const_cast<uchar *>(frame.bits(p));
            inStrides[p] = frame.bytesPerLine(p);
        }
    }
    std::vector<PlaneData> inPlanes;
    stereo_dibr_picture whole;
    stereo_dibr_picture depthWhole;
    if (useGpu) {
        // the planes as they are: the GPU reads the interleaved chroma of NV12 and RGBA whole
        std::memset(&whole, 0, sizeof(whole));
        whole.width = w;
        whole.height = h;
        whole.plane[0] = { inBits[0], inStrides[0], l.bytes, 0, 0, 0 };
        if (l.semiPlanar) {
            whole.nb_planes = 2;
            whole.plane[1] = { inBits[1], inStrides[1], 2 * l.bytes, 1, 1, 0 };
        } else if (l.c[0].kind == Component::Color) {
            whole.nb_planes = 1;
            whole.plane[0].bytes = 4;
        } else {
            whole.nb_planes = 3;
            for (int c = 1; c < 3; ++c)
                whole.plane[c] = { inBits[l.c[c].plane], inStrides[l.c[c].plane], l.bytes,
                                   l.c[c].log2w, l.c[c].log2h, 0 };
        }
        depthWhole = whole;
        depthWhole.nb_planes = 1;
    } else {
        for (int c = 0; c < l.count; ++c)
            inPlanes.push_back(componentPlane(inBits, inStrides, l, c, w, h));
        fillPicture(&whole, inPlanes, l, w, h, fullRange);
        // the depth samples are the first plane of a picture of the depth component alone
        depthWhole = whole;
        depthWhole.nb_planes = 1;
        depthWhole.plane[0] = whole.plane[l.depthComponent];
    }

    stereo_dibr_parts parts, depthParts;
    if (stereo_dibr_unpack(&whole, packing, depthFirst, 0, &parts) < 0
        || stereo_dibr_unpack(&depthWhole, packing, depthFirst, 0, &depthParts) < 0) {
        unmapFrame();
        return Outcome::Failed;
    }

    // A video recorded sideways or upside down: the picture and its depth map are turned as the
    // frame asks (its surface's and its own rotation and mirroring) before the views are drawn, so
    // that the parallax runs along the width of the picture as it is shown and the drawn frame
    // needs no transformation
    const VideoTransformation transformation = qNormalizedFrameTransformation(input);
    std::vector<TurnedPlane> turned;
    turned.reserve(32);
    if (transformation.rotation != QtVideo::Rotation::None
        || transformation.mirroredHorizontallyAfterRotation) {
        parts.picture = turnPicture(parts.picture, transformation, &turned);
        depthParts.depth = turnDepth(depthParts.depth, transformation, &turned);
        if (parts.layer.width) {
            parts.layer = turnPicture(parts.layer, transformation, &turned);
            depthParts.layer_depth = turnDepth(depthParts.layer_depth, transformation, &turned);
        }
    }
    const int vw = parts.picture.width, vh = parts.picture.height;

    stereo_depth_convention c;
    const int known = convention(fileMark, options, l.bits, &c);
    if (known <= 0) {
        unmapFrame();
        return known == 0 ? Outcome::PictureAlone : Outcome::Failed;
    }
    const size_t tableSize = size_t(1) << l.bits;
    table.resize(tableSize);
    if (stereo_depth_parallax_table(&c, vw, table.data()) < 0) {
        unmapFrame();
        return Outcome::Failed;
    }
    layerTable.clear();
    if (parts.layer.width) {
        // a layer's 0 is nothing there (docs/depth-marks.md)
        layerTable = table;
        layerTable[0] = NAN;
    }

    // The matrix of the frame: what it says, and for a frame that does not say, what Qt's own conversion assumes (HD video is BT.709, SD BT.601; the
    // scene graph and the stereo layer's presenter convert the drawn frame with it): the views are then drawn and encoded with the matrix Qt decodes
    // them with, and the output frame says it
    QVideoFrameFormat::ColorSpace matrix = frame.surfaceFormat().colorSpace();
    if (matrix == QVideoFrameFormat::ColorSpace_Undefined)
        matrix = vh > 576 ? QVideoFrameFormat::ColorSpace_BT709 : QVideoFrameFormat::ColorSpace_BT601;
    // the output: the views at the picture's size, side by side
    QVideoFrameFormat format = frame.surfaceFormat();
    format.setColorSpace(matrix);
    if (useGpu && !deepGpu) {
        // RGBA 8 bit, drawn by the GPU
        const QVideoFrameFormat from = format;
        format = QVideoFrameFormat(QSize(2 * vw, vh), PixelFormat::Format_RGBA8888);
        format.setColorSpace(matrix);
        format.setColorTransfer(from.colorTransfer());
        format.setColorRange(QVideoFrameFormat::ColorRange_Full);
        format.setStreamFrameRate(from.streamFrameRate());
        format.setPixelAspectRatio(from.pixelAspectRatio());
        format.setMaxLuminance(from.maxLuminance());
    }
    format.setFrameSize(QSize(2 * vw, vh));
    format.setViewport(QRect(0, 0, 2 * vw, vh));
    format.setStereoMode(Mode::SideBySide);
    format.setStereoFlags({});
    format.setStereoDepthMark({});
    // drawn the right way up: nothing of the frame's transformation is left to apply
    format.setRotation(QtVideo::Rotation::None);
    format.setMirrored(false);
    format.setScanLineDirection(QVideoFrameFormat::TopToBottom);
    if (qToUnderlying(transformation.rotation) % 180 != 0) {
        const QSize par = format.pixelAspectRatio();
        format.setPixelAspectRatio(QSize(par.height(), par.width()));
    }
    // The drawn views reach the sink as textures of the drawing context, which shares the sink's (OpenGL), unless the person asked for the
    // read back or the sink cannot take a texture: then they are read back to system memory, counted and said
    viewsRoute = ViewsRoute::Memory;
    viewsWhy.clear();
    bool texOut = false;
#ifdef QT_MM_DEPTH_GPU
    if (useGpu) {
        if (!options.viewsAsTexture)
            viewsWhy = QStringLiteral("the read back was asked for");
        else if (!sink)
            viewsWhy = QStringLiteral("the video has no sink to show the views yet");
        else if (sink->backend() != QRhi::OpenGLES2)
            viewsWhy = QStringLiteral("the video sink draws with %1, which takes a frame from system memory here")
                               .arg(QString::fromLatin1(sink->backendName()));
        else if (!gpu.sharesSink(&viewsWhy))
            ;
        else if (!gpu.takenBy(*sink))
            viewsWhy = QStringLiteral("the video sink draws in another OpenGL context group than the drawing context");
        else
            texOut = true;
    }
#endif
    QVideoFrame result;
    if (!texOut) {
        result = QVideoFrame(format);
        if (!result.map(QVideoFrame::WriteOnly)) {
            unmapFrame();
            return Outcome::Failed;
        }
    }

    bool knownRate = false;
    const double fps = frameRateOf(input, &knownRate);
    stereo_dibr_params prm;
    stereo_dibr_params_default(&prm);
    prm.view = options.view;
    prm.warp = options.warp;
    prm.fill = options.fill;
    prm.smooth = options.smooth;
    prm.sigma_h = options.sigmaH;
    prm.sigma_v = options.sigmaV;
    prm.radius = options.radius;

#ifdef QT_MM_DEPTH_GPU
    if (useGpu) {
        bool ok = false;
        QString reason;
        GpuDrawer::Views views;
        gpu.run([&] {
            ok = gpu.makeCurrent();
            if (!ok)
                reason = QStringLiteral("the OpenGL context could not be made current");
            // the ways are timed (and judged against the GPU's memory) at the depth of the content
            if (ok)
                stereo_dibr_gl_set_precision(gpu.gl(), deep.on ? deep.precision : 8);
            if (ok && !chooseWay(options, true, vw, vh, fps, &prm.warp, &prm.fill)) {
                ok = false;
                reason = QStringLiteral("the ways of drawing could not be timed on the GPU");
            }
            if (ok)
                ok = gpu.draw(source, deep, fullRange,
                              matrix == QVideoFrameFormat::ColorSpace_BT709 ? 1 : matrix == QVideoFrameFormat::ColorSpace_BT2020 ? 2 : 0,
                              parts, depthParts, prm, table.data(), int(tableSize),
                              layerTable.empty() ? nullptr : layerTable.data(), texOut ? nullptr : &result,
                              texOut ? &views : nullptr, &reason);
            gpu.releaseImport();
            if (ok && (!gpuMem.valid || gpuMem.width != vw || gpuMem.height != vh || memoryFrames++ % 600 == 0)) {
                // the driver's free memory, once a size and every few seconds
                gpuMem.valid = true;
                gpuMem.freeBytes = gpu.freeMemory();
                gpuMem.width = vw;
                gpuMem.height = vh;
                gpuMem.bits = deep.on ? deep.precision : 8;
            }
            gpu.doneCurrent();
            importLive = false;
        });
        result.unmap();
        unmapFrame();
        if (!ok) {
            *why = reason;
            return Outcome::GpuFailed;
        }
        onGpu = true;
        if (choice.valid && !knownRate)
            choice.fps = fps;
        if (texOut) {
            *out = QVideoFramePrivate::createFrame(
                    std::make_unique<DrawnViewsBuffer>(gpuOwner, views, format, viewReadBacks), format);
            viewsRoute = ViewsRoute::Texture;
            sayViews(QStringLiteral("the drawn views go to the video sink as textures of the GPU, shared with the sink's context: "
                                    "no read back to system memory"));
        } else {
            ++*viewReadBacks;
            viewsRoute = ViewsRoute::ReadBack;
            sayViews(QStringLiteral("the drawn views are read back to system memory: %1").arg(viewsWhy));
            *out = result;
        }
        out->setStartTime(input.startTime());
        out->setEndTime(input.endTime());
        return Outcome::Drawn;
    }
#endif

    // the C code, on the processor
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

    if (!chooseWay(options, false, vw, vh, fps, &prm.warp, &prm.fill)) {
        result.unmap();
        unmapFrame();
        return Outcome::Failed;
    }
    int r;
    if (parts.layer.width) {
        r = stereo_dibr_render_layer(dibr, &prm, table.data(), int(tableSize), &parts.picture,
                                     &depthParts.depth, parts.layer_role, &parts.layer,
                                     layerTable.data(), int(tableSize), &depthParts.layer_depth,
                                     &left, &right);
    } else {
        r = stereo_dibr_render(dibr, &prm, table.data(), int(tableSize), &parts.picture,
                               &depthParts.depth, &left, &right);
    }
    for (int cc = 0; cc < l.count; ++cc)
        putComponent(outBits, outStrides, l, cc, 2 * vw, vh, outPlanes[cc]);
    result.unmap();
    unmapFrame();
    if (r < 0)
        return Outcome::Failed;
    onGpu = false;
    viewsRoute = ViewsRoute::Memory;
    viewsWhy = QStringLiteral("the views are drawn on the processor, in system memory");
    result.setStartTime(input.startTime());
    result.setEndTime(input.endTime());
    *out = result;
    return Outcome::Drawn;
}

Renderer::Renderer() : d(std::make_unique<Private>())
{
    d->dibr = stereo_dibr_alloc();
}

Renderer::~Renderer() = default;

Choice Renderer::choice() const
{
    return d->choice;
}

bool Renderer::lastOnGpu() const
{
    return d->onGpu;
}

Renderer::FrameRoute Renderer::lastFrameRoute() const
{
    return d->route;
}

GpuMemory Renderer::gpuMemory() const
{
    return d->onGpu ? d->gpuMem : GpuMemory();
}

QString Renderer::lastFrameRouteWhy() const
{
    return d->routeWhy;
}

int Renderer::hardwareReadBacks() const
{
    return d->hwReadBacks;
}

int Renderer::importChecks(int *empty) const
{
    if (empty)
        *empty = d->gpu.importEmpties();
    return d->gpu.importChecks();
}

void Renderer::setSinkRhi(QRhi *rhi)
{
    d->sink = rhi;
}

Renderer::ViewsRoute Renderer::lastViewsRoute() const
{
    return d->viewsRoute;
}

QString Renderer::lastViewsRouteWhy() const
{
    return d->viewsWhy;
}

int Renderer::viewsReadBacks() const
{
    return *d->viewReadBacks;
}

Renderer::Result Renderer::draw(const QVideoFrame &input, Mode mode, bool depthFirst,
                                const QString &fileMark, const Options &options, QVideoFrame *out)
{
    if (packingOf(mode) < 0 || !d->dibr || !input.isValid())
        return Result::Failed;

    // Where the views are drawn: on the GPU when it can and the person has not asked for the
    // processor; the C code on the processor otherwise, which the log says (it is a route of the
    // design, never a silent one)
    QString cpuReason;
    bool tryGpu = false;
#ifdef QT_MM_DEPTH_GPU
    if (!options.gpu) {
        cpuReason = QStringLiteral("the processor was asked for");
    } else {
        QString why;
        bool made = false;
        // the drawing context shares the video sink's when the sink draws with OpenGL
        QOpenGLContext *share = nullptr;
        if (d->sink && d->sink->backend() == QRhi::OpenGLES2) {
            const auto *handles = static_cast<const QRhiGles2NativeHandles *>(d->sink->nativeHandles());
            share = handles ? handles->context : nullptr;
        }
        d->gpu.run([&] { made = d->gpu.ensure(&why, share); });
        if (made)
            tryGpu = true;
        else
            cpuReason = why;
    }
#else
    cpuReason = QStringLiteral("this Qt Multimedia has no OpenGL");
#endif
    Private::Outcome o = Private::Outcome::GpuFailed;
    if (tryGpu) {
        QString why;
        o = d->render(input, mode, depthFirst, fileMark, options, true, out, &why);
        if (o == Private::Outcome::GpuFailed)
            cpuReason = why;
        else if (o == Private::Outcome::Drawn)
            d->say(QStringLiteral("the views of a depth video are drawn on the GPU: %1")
                           .arg(d->choice.valid ? d->choice.gpu : QStringLiteral("the way is the person's")),
                   false);
    }
    if (o == Private::Outcome::GpuFailed) {
        d->say(QStringLiteral("the views of a depth video are drawn by the C code on the processor, "
                              "not on the GPU: %1")
                       .arg(cpuReason),
               true);
        QString why;
        o = d->render(input, mode, depthFirst, fileMark, options, false, out, &why);
#ifdef QT_MM_DEPTH_GPU
        // a GPU that was reset is said in the choice, so that the controls say it
        if (o == Private::Outcome::Drawn && d->gpu.lost())
            d->choice.gpuLost = true;
#endif
    }
    switch (o) {
    case Private::Outcome::Drawn:
        return Result::Drawn;
    case Private::Outcome::PictureAlone:
        return Result::PictureAlone;
    default:
        return Result::Failed;
    }
}

#else

struct Renderer::Private
{
};

Renderer::Renderer() : d(std::make_unique<Private>()) { }

Renderer::~Renderer() = default;

Choice Renderer::choice() const
{
    return {};
}

bool Renderer::lastOnGpu() const
{
    return false;
}

Renderer::FrameRoute Renderer::lastFrameRoute() const
{
    return FrameRoute::Memory;
}

GpuMemory Renderer::gpuMemory() const
{
    return {};
}

QString Renderer::lastFrameRouteWhy() const
{
    return {};
}

int Renderer::hardwareReadBacks() const
{
    return 0;
}

int Renderer::importChecks(int *empty) const
{
    if (empty)
        *empty = 0;
    return 0;
}

void Renderer::setSinkRhi(QRhi *)
{
}

Renderer::ViewsRoute Renderer::lastViewsRoute() const
{
    return ViewsRoute::Memory;
}

QString Renderer::lastViewsRouteWhy() const
{
    return {};
}

int Renderer::viewsReadBacks() const
{
    return 0;
}

Renderer::Result Renderer::draw(const QVideoFrame &, Mode, bool, const QString &, const Options &,
                                QVideoFrame *)
{
    return Result::Failed;
}

#endif

} // namespace QQuickVideoDepth

QT_END_NAMESPACE
