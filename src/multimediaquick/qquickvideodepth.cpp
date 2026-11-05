// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideodepth_p.h"

#include <QtMultimedia/private/qmultimediautils_p.h>
#include <QtMultimedia/private/qvideoframeconverter_p.h>
#include <QtMultimedia/private/qvideotransformation_p.h>
#include <QtGui/qimage.h>

#include <QtCore/qloggingcategory.h>
#include <QtCore/qthread.h>
#include <QtGui/qtguiglobal.h>

#ifdef QT_MM_STEREO_DIBR
#  include <stereo-dibr.h>
#  include <stereo-dibr-gl.h>
#  if QT_CONFIG(opengl)
#    include <QtGui/qopenglcontext.h>
#    include <QtGui/qopenglextrafunctions.h>
#    include <QtGui/qoffscreensurface.h>
#    include <QtGui/qsurfaceformat.h>
#    define QT_MM_DEPTH_GPU
#  endif
#endif

#include <array>
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
                   GL_COLOR_ATTACHMENT0_ = 0x8CE0, GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;

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
out vec4 o_color;
void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy);
    float y = texelFetch(u_y, at, 0).r;
    ivec2 c = ivec2(at.x >> u_shift.x, at.y >> u_shift.y);
    float u, v;
    if (u_mode == 0) {
        u = texelFetch(u_u, c, 0).r;
        v = texelFetch(u_v, c, 0).r;
    } else {
        vec2 uv = texelFetch(u_u, c, 0).rg;
        u = u_mode == 1 ? uv.r : uv.g;
        v = u_mode == 1 ? uv.g : uv.r;
    }
    float yy, cb, cr;
    if (u_full != 0) {
        yy = y;
        cb = (u * 255.0 - 128.0) / 255.0;
        cr = (v * 255.0 - 128.0) / 255.0;
    } else {
        yy = (y * 255.0 - 16.0) / 219.0;
        cb = (u * 255.0 - 128.0) / 224.0;
        cr = (v * 255.0 - 128.0) / 224.0;
    }
    float r = yy + 2.0 * (1.0 - u_k.x) * cr;
    float b = yy + 2.0 * (1.0 - u_k.y) * cb;
    float g = (yy - u_k.x * r - u_k.y * b) / (1.0 - u_k.x - u_k.y);
    o_color = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
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

class GpuDrawer
{
public:
    enum class Source { Planar, SemiUV, SemiVU, Rgba };

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
    bool ensure(QString *why)
    {
        if (m_lost) {
            *why = QStringLiteral("the GPU was reset while drawing, it is not used again for this video");
            return false;
        }
        if (m_gl && m_thread == QThread::currentThread())
            return true;
        if (m_failedThread == QThread::currentThread()) {
            *why = m_whyNot;
            return false;
        }
        destroy();
        m_thread = QThread::currentThread();
        auto context = std::make_unique<QOpenGLContext>();
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        // a context that tells when the GPU was reset (KHR_robustness): the library watches it (stereo_dibr_gl_reset_status())
        format.setOption(QSurfaceFormat::ResetNotification);
        context->setFormat(format);
        if (!context->create()) {
            // OpenGL ES 3.0, for a platform that has no desktop OpenGL
            format = QSurfaceFormat();
            format.setRenderableType(QSurfaceFormat::OpenGLES);
            format.setVersion(3, 0);
            format.setOption(QSurfaceFormat::ResetNotification);
            context->setFormat(format);
            if (!context->create())
                return fail(QStringLiteral("no OpenGL 3.3 or OpenGL ES 3.0 context could be made"), why);
        }
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
        m_f = m_context->extraFunctions();
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
    bool lost() const { return m_lost; }
    bool isEs() const { return m_context && m_context->isOpenGLES(); }

    // The views of a picture and its depth map into result (RGBA 8 bit, twice the picture's
    // width), in the current context. parts are the picture and its depth map (stereo_dibr_unpack()
    // of the frame, turned as the frame asks), dparts the same for the depth component of the
    // frame. Returns false with the reason.
    bool draw(Source source, bool fullRange, int colorSpace, const stereo_dibr_parts &parts,
              const stereo_dibr_parts &dparts, const stereo_dibr_params &prm, const float *table,
              int tableSize, const float *layerTable, QVideoFrame *result, QString *why)
    {
        const int vw = parts.picture.width, vh = parts.picture.height;
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
        makeTexture(Target, vw * 2, vh, GL_RGBA8_, GL_RGBA_);
        const float hole[4] = { 0.f, 0.f, 0.f, 1.f };
        int r;
        if (layered)
            r = stereo_dibr_gl_render_layer(m_gl, &prm, table, tableSize, m_tex[Pic].id, vw, vh,
                                            m_tex[Depth].id, dparts.depth.width,
                                            dparts.depth.height, 0, parts.layer_role,
                                            m_tex[LayerPic].id, layerTable, tableSize,
                                            m_tex[LayerDepth].id, dparts.layer_depth.width,
                                            dparts.layer_depth.height, m_tex[Target].id, hole);
        else
            r = stereo_dibr_gl_render(m_gl, &prm, table, tableSize, m_tex[Pic].id, vw, vh,
                                      m_tex[Depth].id, dparts.depth.width, dparts.depth.height, 0,
                                      m_tex[Target].id, hole);
        if (r < 0) {
            if (r == -4 || stereo_dibr_gl_reset_status(m_gl))
                m_lost = true;
            *why = QString::fromLatin1(stereo_dibr_gl_error(m_gl));
            if (why->isEmpty())
                *why = QStringLiteral("the views could not be drawn");
            return false;
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
    enum TexId { Y, U, V, Raw, Pic, Depth, Target, LayerPic, LayerDepth, NbTextures };
    struct Tex
    {
        unsigned id = 0;
        int w = 0, h = 0;
        unsigned internal = 0;
    };

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
        if (m_context && m_surface && m_context->makeCurrent(m_surface.get())) {
            g_depthContext = m_context.get();
            if (m_gl)
                stereo_dibr_gl_free(m_gl);
            for (Tex &t : m_tex)
                if (t.id)
                    m_f->glDeleteTextures(1, &t.id);
            if (m_vao)
                m_f->glDeleteVertexArrays(1, &m_vao);
            if (m_fbo)
                m_f->glDeleteFramebuffers(1, &m_fbo);
            if (m_convert)
                m_f->glDeleteProgram(m_convert);
            if (m_channel)
                m_f->glDeleteProgram(m_channel);
            m_context->doneCurrent();
        }
        m_gl = nullptr;
        m_tex = {};
        m_vao = m_fbo = m_convert = m_channel = 0;
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
        return m_convert && m_channel;
    }

    void makeTexture(TexId id, int w, int h, unsigned internal, unsigned format)
    {
        Tex &t = m_tex[id];
        if (!t.id)
            m_f->glGenTextures(1, &t.id);
        m_f->glBindTexture(GL_TEXTURE_2D_, t.id);
        if (t.w != w || t.h != h || t.internal != internal) {
            m_f->glTexImage2D(GL_TEXTURE_2D_, 0, int(internal), w, h, 0, format, GL_UNSIGNED_BYTE_,
                              nullptr);
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
        if (!upload(Y, w, h, 1, py.data, py.stride))
            return false;
        if (source == Source::Planar) {
            const stereo_dibr_plane &pv = pic.plane[2];
            if (!upload(U, cw, ch, 1, pu.data, pu.stride) || !upload(V, cw, ch, 1, pv.data, pv.stride))
                return false;
        } else if (!upload(U, cw, ch, 2, pu.data, pu.stride)) {
            return false;
        }
        makeTexture(out, w, h, GL_RGBA8_, GL_RGBA_);
        m_f->glUseProgram(m_convert);
        bindTexture(0, Y, m_convert, "u_y");
        bindTexture(1, U, m_convert, "u_u");
        bindTexture(2, source == Source::Planar ? V : U, m_convert, "u_v");
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
        pass(m_convert, out, w, h);
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
    unsigned m_vao = 0, m_fbo = 0, m_convert = 0, m_channel = 0;
    bool m_lost = false;
};

#endif // QT_MM_DEPTH_GPU

} // namespace

struct Renderer::Private
{
    enum class Outcome { Drawn, PictureAlone, Failed, GpuFailed };

    stereo_dibr *dibr = nullptr;
    std::vector<float> table, layerTable;
    std::vector<uint16_t> lut;
#ifdef QT_MM_DEPTH_GPU
    GpuDrawer gpu;
#endif
    // what the automatic way chose for the last frame, and where it was drawn
    Choice choice;
    bool onGpu = false;
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
bool gpuSourceOf(PixelFormat format, GpuDrawer::Source *source)
{
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
#ifdef QT_MM_DEPTH_GPU
    GpuDrawer::Source source = GpuDrawer::Source::Planar;
    if (useGpu && (l.bits != 8 || !gpuSourceOf(frame.pixelFormat(), &source))) {
        *why = QStringLiteral("the pixel format %1 is not one the GPU reads")
                       .arg(QVideoFrameFormat::pixelFormatToString(frame.pixelFormat()));
        return Outcome::GpuFailed;
    }
#else
    Q_UNUSED(why);
    if (useGpu)
        return Outcome::GpuFailed;
#endif
    if (!frame.map(QVideoFrame::ReadOnly))
        return Outcome::Failed;

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
            whole.plane[1] = { inBits[1], inStrides[1], 2, 1, 1, 0 };
        } else if (l.c[0].kind == Component::Color) {
            whole.nb_planes = 1;
            whole.plane[0].bytes = 4;
        } else {
            whole.nb_planes = 3;
            for (int c = 1; c < 3; ++c)
                whole.plane[c] = { inBits[l.c[c].plane], inStrides[l.c[c].plane], 1,
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
        frame.unmap();
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
        frame.unmap();
        return known == 0 ? Outcome::PictureAlone : Outcome::Failed;
    }
    const size_t tableSize = size_t(1) << l.bits;
    table.resize(tableSize);
    if (stereo_depth_parallax_table(&c, vw, table.data()) < 0) {
        frame.unmap();
        return Outcome::Failed;
    }
    layerTable.clear();
    if (parts.layer.width) {
        // a layer's 0 is nothing there (docs/depth-marks.md)
        layerTable = table;
        layerTable[0] = NAN;
    }

    // the output: the views at the picture's size, side by side
    QVideoFrameFormat format = frame.surfaceFormat();
    if (useGpu) {
        // RGBA 8 bit, drawn by the GPU
        const QVideoFrameFormat from = format;
        format = QVideoFrameFormat(QSize(2 * vw, vh), PixelFormat::Format_RGBA8888);
        format.setColorSpace(from.colorSpace());
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
    QVideoFrame result(format);
    if (!result.map(QVideoFrame::WriteOnly)) {
        frame.unmap();
        return Outcome::Failed;
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
        gpu.run([&] {
            ok = gpu.makeCurrent();
            if (!ok)
                reason = QStringLiteral("the OpenGL context could not be made current");
            if (ok && !chooseWay(options, true, vw, vh, fps, &prm.warp, &prm.fill)) {
                ok = false;
                reason = QStringLiteral("the ways of drawing could not be timed on the GPU");
            }
            if (ok)
                ok = gpu.draw(source, fullRange,
                              frame.surfaceFormat().colorSpace()
                                      == QVideoFrameFormat::ColorSpace_BT709
                                      ? 1
                                      : frame.surfaceFormat().colorSpace()
                                              == QVideoFrameFormat::ColorSpace_BT2020
                                              ? 2
                                              : 0,
                              parts, depthParts, prm, table.data(), int(tableSize),
                              layerTable.empty() ? nullptr : layerTable.data(), &result, &reason);
            gpu.doneCurrent();
        });
        result.unmap();
        frame.unmap();
        if (!ok) {
            *why = reason;
            return Outcome::GpuFailed;
        }
        onGpu = true;
        if (choice.valid && !knownRate)
            choice.fps = fps;
        result.setStartTime(input.startTime());
        result.setEndTime(input.endTime());
        *out = result;
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
        frame.unmap();
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
    frame.unmap();
    if (r < 0)
        return Outcome::Failed;
    onGpu = false;
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
        d->gpu.run([&] { made = d->gpu.ensure(&why); });
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

Renderer::Result Renderer::draw(const QVideoFrame &, Mode, bool, const QString &, const Options &,
                                QVideoFrame *)
{
    return Result::Failed;
}

#endif

} // namespace QQuickVideoDepth

QT_END_NAMESPACE
