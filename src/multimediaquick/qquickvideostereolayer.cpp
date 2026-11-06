// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideostereolayer_p.h"

#ifdef QT_MM_STEREO_LAYER

#include <QtCore/qloggingcategory.h>
#include <QtCore/qmutex.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qwindow.h>
#include <QtGui/qpa/qplatformwindow_p.h>

#include <QtCore/qlibrary.h>
#include <QtCore/qfile.h>

#define WL_EGL_PLATFORM 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <stereo-declare.h>

#include "wayland-viewporter-client-protocol.h"
#include "wayland-color-management-v1-client-protocol.h"

QT_BEGIN_NAMESPACE

Q_STATIC_LOGGING_CATEGORY(qLcStereoLayer, "qt.multimedia.video.stereolayer")

using namespace QQuickVideoStereo;
using namespace Qt::StringLiterals;

namespace {

constexpr uint32_t shmXrgb8888 = 1;          // WL_SHM_FORMAT_XRGB8888
constexpr uint32_t shmXrgb2101010 = 0x30335258; // 'XR30'
constexpr uint32_t shmXbgr16 = 0x38344258;      // 'XB48'
constexpr uint32_t shmXbgr16f = 0x48344258;     // 'XB4H'

uint32_t shmCode(BufferFormat format)
{
    switch (format) {
    case BufferFormat::Xrgb8888: return shmXrgb8888;
    case BufferFormat::Xrgb2101010: return shmXrgb2101010;
    case BufferFormat::Xbgr16161616: return shmXbgr16;
    case BufferFormat::Xbgr16161616f: return shmXbgr16f;
    }
    return shmXrgb8888;
}

struct Globals
{
    wl_compositor *compositor = nullptr;
    wl_subcompositor *subcompositor = nullptr;
    wl_shm *shm = nullptr;
    wp_viewporter *viewporter = nullptr;
    wp_color_manager_v1 *colorManager = nullptr;
    bool stereo = false;
    bool dmabuf = false;
    QList<uint32_t> shmFormats;
    // what the color manager supports
    bool parametric = false;
    bool masteringVolume = false;
    QList<uint32_t> namedTransfers;
    QList<uint32_t> namedPrimaries;
};

void shmFormat(void *data, wl_shm *, uint32_t format)
{
    static_cast<Globals *>(data)->shmFormats.append(format);
}
const wl_shm_listener shmListener = { shmFormat };

void cmIntent(void *, wp_color_manager_v1 *, uint32_t) { }
void cmFeature(void *data, wp_color_manager_v1 *, uint32_t feature)
{
    if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)
        static_cast<Globals *>(data)->parametric = true;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES)
        static_cast<Globals *>(data)->masteringVolume = true;
}
void cmTf(void *data, wp_color_manager_v1 *, uint32_t tf) { static_cast<Globals *>(data)->namedTransfers.append(tf); }
void cmPrimaries(void *data, wp_color_manager_v1 *, uint32_t p) { static_cast<Globals *>(data)->namedPrimaries.append(p); }
void cmDone(void *, wp_color_manager_v1 *) { }
const wp_color_manager_v1_listener colorManagerListener = { cmIntent, cmFeature, cmTf, cmPrimaries, cmDone };

void registryGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    auto *g = static_cast<Globals *>(data);
    if (!strcmp(interface, wl_compositor_interface.name)) {
        g->compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, qMin(version, 4u)));
    } else if (!strcmp(interface, wl_subcompositor_interface.name)) {
        g->subcompositor = static_cast<wl_subcompositor *>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        g->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        wl_shm_add_listener(g->shm, &shmListener, g);
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        g->viewporter = static_cast<wp_viewporter *>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (!strcmp(interface, wp_color_manager_v1_interface.name)) {
        g->colorManager = static_cast<wp_color_manager_v1 *>(wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1));
        wp_color_manager_v1_add_listener(g->colorManager, &colorManagerListener, g);
    } else if (!strcmp(interface, "kde_stereo_content_manager_v1")) {
        g->stereo = true;
    } else if (!strcmp(interface, "zwp_linux_dmabuf_v1")) {
        g->dmabuf = true;
    }
}
void registryGlobalRemove(void *, wl_registry *, uint32_t) { }
const wl_registry_listener registryListener = { registryGlobal, registryGlobalRemove };

wl_display *waylandDisplay()
{
    if (QGuiApplication::platformName() != QLatin1String("wayland"))
        return nullptr;
    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    return app ? app->display() : nullptr;
}

// The globals are read on a queue of their own, so that nothing is dispatched on Qt's queue
wl_registry *readGlobals(wl_display *display, wl_event_queue *queue, Globals &globals)
{
    auto *wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
    wl_registry *registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &registryListener, &globals);
    wl_display_roundtrip_queue(display, queue);
    // the events of wl_shm and of the color manager come after the globals
    wl_display_roundtrip_queue(display, queue);
    return registry;
}

void destroyGlobals(Globals &g)
{
    if (g.colorManager)
        wp_color_manager_v1_destroy(g.colorManager);
    if (g.compositor)
        wl_compositor_destroy(g.compositor);
    if (g.subcompositor)
        wl_subcompositor_destroy(g.subcompositor);
    if (g.shm)
        wl_shm_destroy(g.shm);
    if (g.viewporter)
        wp_viewporter_destroy(g.viewporter);
    g = {};
}

struct Buffer
{
    wl_buffer *buffer = nullptr;
    uchar *data = nullptr;
    size_t size = 0;
    QSize pixels;
    uint32_t format = 0;
    bool busy = false;
};

void bufferRelease(void *data, wl_buffer *)
{
    static_cast<Buffer *>(data)->busy = false;
}
const wl_buffer_listener bufferListener = { bufferRelease };

void destroyBuffer(Buffer &b)
{
    if (b.buffer)
        wl_buffer_destroy(b.buffer);
    if (b.data)
        munmap(b.data, b.size);
    b = {};
}

bool createBuffer(Buffer &b, wl_shm *shm, wl_event_queue *queue, QSize pixels, BufferFormat format)
{
    const int stride = pixels.width() * bytesPerPixel(format);
    const size_t size = size_t(stride) * pixels.height();
    const int fd = memfd_create("qt-video-stereo-layer", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    if (ftruncate(fd, off_t(size)) < 0) {
        close(fd);
        return false;
    }
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return false;
    }
    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, int(size));
    close(fd);
    wl_buffer *wlBuffer = wl_shm_pool_create_buffer(pool, 0, pixels.width(), pixels.height(), stride, shmCode(format));
    wl_shm_pool_destroy(pool);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wlBuffer), queue);
    b.buffer = wlBuffer;
    b.data = static_cast<uchar *>(data);
    b.size = size;
    b.pixels = pixels;
    b.format = shmCode(format);
    b.busy = false;
    wl_buffer_add_listener(wlBuffer, &bufferListener, &b);
    return true;
}

void imageDescriptionFailed(void *data, wp_image_description_v1 *, uint32_t, const char *msg)
{
    qCWarning(qLcStereoLayer) << "the compositor refused the color description:" << msg;
    *static_cast<int *>(data) = -1;
}
void imageDescriptionReady(void *data, wp_image_description_v1 *, uint32_t)
{
    *static_cast<int *>(data) = 1;
}
const wp_image_description_v1_listener imageDescriptionListener = { imageDescriptionFailed, imageDescriptionReady };

} // namespace

// ---- the views drawn into a window surface of the layer's own (no read back) ---------------------------------------------------------------

// The EGL and wayland-egl entry points, loaded when first needed (the library links neither)
struct EglApi
{
    EGLBoolean (*bindAPI)(EGLenum) = nullptr;
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = nullptr;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = nullptr;
    EGLSurface (*createWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *) = nullptr;
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    EGLBoolean (*swapBuffers)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (*swapInterval)(EGLDisplay, EGLint) = nullptr;
    EGLBoolean (*destroySurface)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (*destroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLint (*getError)() = nullptr;
    EGLBoolean (*getConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *) = nullptr;
    EGLBoolean (*getConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *) = nullptr;
    void *(*getProcAddress)(const char *) = nullptr;
    wl_egl_window *(*windowCreate)(wl_surface *, int, int) = nullptr;
    void (*windowResize)(wl_egl_window *, int, int, int, int) = nullptr;
    void (*windowDestroy)(wl_egl_window *) = nullptr;
    bool ok = false;

    static const EglApi &get()
    {
        static const EglApi api = [] {
            EglApi a;
            static QLibrary egl(QStringLiteral("EGL"), 1), wegl(QStringLiteral("wayland-egl"), 1);
            if (!egl.load() || !wegl.load())
                return a;
// by the process's own symbol lookup first, so that what is preloaded into the program (a recorder, a debugging layer) sees these calls as it sees
// Qt's own, and by the library's handle where the lookup finds nothing
#define QT_MM_RESOLVE(lib, member, name)                                                                         \
    do {                                                                                                         \
        void *symbol = dlsym(RTLD_DEFAULT, name);                                                                \
        a.member = reinterpret_cast<decltype(a.member)>(symbol ? symbol : reinterpret_cast<void *>(lib.resolve(name))); \
    } while (0)
            QT_MM_RESOLVE(egl, bindAPI, "eglBindAPI");
            QT_MM_RESOLVE(egl, chooseConfig, "eglChooseConfig");
            QT_MM_RESOLVE(egl, createContext, "eglCreateContext");
            QT_MM_RESOLVE(egl, createWindowSurface, "eglCreateWindowSurface");
            QT_MM_RESOLVE(egl, makeCurrent, "eglMakeCurrent");
            QT_MM_RESOLVE(egl, swapBuffers, "eglSwapBuffers");
            QT_MM_RESOLVE(egl, swapInterval, "eglSwapInterval");
            QT_MM_RESOLVE(egl, destroySurface, "eglDestroySurface");
            QT_MM_RESOLVE(egl, destroyContext, "eglDestroyContext");
            QT_MM_RESOLVE(egl, getError, "eglGetError");
            QT_MM_RESOLVE(egl, getConfigs, "eglGetConfigs");
            QT_MM_RESOLVE(egl, getConfigAttrib, "eglGetConfigAttrib");
            QT_MM_RESOLVE(egl, getProcAddress, "eglGetProcAddress");
            QT_MM_RESOLVE(wegl, windowCreate, "wl_egl_window_create");
            QT_MM_RESOLVE(wegl, windowResize, "wl_egl_window_resize");
            QT_MM_RESOLVE(wegl, windowDestroy, "wl_egl_window_destroy");
#undef QT_MM_RESOLVE
            a.ok = a.bindAPI && a.chooseConfig && a.createContext && a.createWindowSurface && a.makeCurrent && a.swapBuffers
                    && a.swapInterval && a.destroySurface && a.destroyContext && a.getError && a.getProcAddress && a.windowCreate
                    && a.windowResize && a.windowDestroy && a.getConfigs && a.getConfigAttrib;
            return a;
        }();
        return api;
    }
};

// The OpenGL entry points of the one pass, from the context the layer makes
struct GlApi
{
    using Enum = unsigned;
    unsigned (*createShader)(Enum) = nullptr;
    void (*shaderSource)(unsigned, int, const char *const *, const int *) = nullptr;
    void (*compileShader)(unsigned) = nullptr;
    void (*getShaderiv)(unsigned, Enum, int *) = nullptr;
    unsigned (*createProgram)() = nullptr;
    void (*attachShader)(unsigned, unsigned) = nullptr;
    void (*linkProgram)(unsigned) = nullptr;
    void (*getProgramiv)(unsigned, Enum, int *) = nullptr;
    void (*deleteShader)(unsigned) = nullptr;
    void (*useProgram)(unsigned) = nullptr;
    int (*getUniformLocation)(unsigned, const char *) = nullptr;
    void (*uniform1i)(int, int) = nullptr;
    void (*uniform2i)(int, int, int) = nullptr;
    void (*activeTexture)(Enum) = nullptr;
    void (*bindTexture)(Enum, unsigned) = nullptr;
    void (*drawArrays)(Enum, int, int) = nullptr;
    void (*viewport)(int, int, int, int) = nullptr;
    void (*genVertexArrays)(int, unsigned *) = nullptr;
    void (*bindVertexArray)(unsigned) = nullptr;
    void (*finish)() = nullptr;
    void (*disable)(Enum) = nullptr;
    void (*readPixels)(int, int, int, int, Enum, Enum, void *) = nullptr;
    bool ok = false;

    bool load(const EglApi &egl)
    {
#define QT_MM_GL(member, name) member = reinterpret_cast<decltype(member)>(egl.getProcAddress(name))
        QT_MM_GL(createShader, "glCreateShader");
        QT_MM_GL(shaderSource, "glShaderSource");
        QT_MM_GL(compileShader, "glCompileShader");
        QT_MM_GL(getShaderiv, "glGetShaderiv");
        QT_MM_GL(createProgram, "glCreateProgram");
        QT_MM_GL(attachShader, "glAttachShader");
        QT_MM_GL(linkProgram, "glLinkProgram");
        QT_MM_GL(getProgramiv, "glGetProgramiv");
        QT_MM_GL(deleteShader, "glDeleteShader");
        QT_MM_GL(useProgram, "glUseProgram");
        QT_MM_GL(getUniformLocation, "glGetUniformLocation");
        QT_MM_GL(uniform1i, "glUniform1i");
        QT_MM_GL(uniform2i, "glUniform2i");
        QT_MM_GL(activeTexture, "glActiveTexture");
        QT_MM_GL(bindTexture, "glBindTexture");
        QT_MM_GL(drawArrays, "glDrawArrays");
        QT_MM_GL(viewport, "glViewport");
        QT_MM_GL(genVertexArrays, "glGenVertexArrays");
        QT_MM_GL(bindVertexArray, "glBindVertexArray");
        QT_MM_GL(finish, "glFinish");
        QT_MM_GL(disable, "glDisable");
        QT_MM_GL(readPixels, "glReadPixels");
#undef QT_MM_GL
        ok = createShader && shaderSource && compileShader && getShaderiv && createProgram && attachShader && linkProgram && getProgramiv
                && deleteShader && useProgram && getUniformLocation && uniform1i && uniform2i && activeTexture && bindTexture && drawArrays
                && viewport && genVertexArrays && bindVertexArray && finish && disable && readPixels;
        return ok;
    }
};

// The window surface formats a display has, as "r.g.b.a" (with f for floating point), for the message of a refusal
QString windowFormatsOf(EGLDisplay display)
{
    const EglApi &egl = EglApi::get();
    EGLint count = 0;
    egl.getConfigs(display, nullptr, 0, &count);
    QList<EGLConfig> configs(count);
    egl.getConfigs(display, configs.data(), count, &count);
    QStringList formats;
    for (int i = 0; i < count; ++i) {
        EGLint surface = 0, r = 0, g = 0, b = 0, a = 0, type = 0;
        egl.getConfigAttrib(display, configs[i], EGL_SURFACE_TYPE, &surface);
        if (!(surface & EGL_WINDOW_BIT))
            continue;
        egl.getConfigAttrib(display, configs[i], EGL_RED_SIZE, &r);
        egl.getConfigAttrib(display, configs[i], EGL_GREEN_SIZE, &g);
        egl.getConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &b);
        egl.getConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &a);
        egl.getConfigAttrib(display, configs[i], EGL_COLOR_COMPONENT_TYPE_EXT, &type);
        const QString text = QStringLiteral("%1.%2.%3.%4%5").arg(r).arg(g).arg(b).arg(a).arg(type == EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT ? u"f"_s : u""_s);
        if (!formats.contains(text))
            formats << text;
    }
    return formats.join(u' ');
}

// What the window surface of a format asks of the driver
bool configAttributes(BufferFormat format, bool es, EGLint *out, int capacity)
{
    int n = 0;
    auto put = [&](EGLint k, EGLint v) { out[n++] = k; out[n++] = v; };
    Q_ASSERT(capacity >= 24);
    put(EGL_SURFACE_TYPE, EGL_WINDOW_BIT);
    put(EGL_RENDERABLE_TYPE, es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT);
    switch (format) {
    case BufferFormat::Xrgb8888:
        put(EGL_RED_SIZE, 8);
        put(EGL_GREEN_SIZE, 8);
        put(EGL_BLUE_SIZE, 8);
        put(EGL_ALPHA_SIZE, 0);
        break;
    case BufferFormat::Xrgb2101010:
        put(EGL_RED_SIZE, 10);
        put(EGL_GREEN_SIZE, 10);
        put(EGL_BLUE_SIZE, 10);
        put(EGL_ALPHA_SIZE, 2);
        break;
    case BufferFormat::Xbgr16161616f:
        put(EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT);
        put(EGL_RED_SIZE, 16);
        put(EGL_GREEN_SIZE, 16);
        put(EGL_BLUE_SIZE, 16);
        put(EGL_ALPHA_SIZE, 16);
        break;
    case BufferFormat::Xbgr16161616:
        return false; // no window surface of 16 bit unsigned normalised in EGL
    }
    out[n] = EGL_NONE;
    return true;
}

struct GpuSurface
{
    using Enum = unsigned;
    EGLDisplay display = nullptr;
    EGLContext shareContext = nullptr;
    EGLContext context = nullptr;
    EGLConfig config = nullptr;
    EGLSurface surface = nullptr;
    wl_egl_window *window = nullptr;
    bool es = false;
    BufferFormat format = BufferFormat::Xrgb8888;
    QSize size;
    GlApi gl;
    unsigned program = 0, vao = 0;
    int uSrc = -1, uCrop = -1, uEye = -1, uSwap = -1, uWidth = -1;
    bool current = false;
    int dumped = 0;

    bool active() const { return surface != nullptr; }

    // The surface and the window, kept: the context and the program stay for the next one
    void dropSurface()
    {
        const EglApi &egl = EglApi::get();
        if (!egl.ok)
            return;
        if (current) {
            egl.makeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            current = false;
        }
        if (surface)
            egl.destroySurface(display, surface);
        surface = nullptr;
        if (window)
            egl.windowDestroy(window);
        window = nullptr;
        size = {};
    }

    void dropAll()
    {
        dropSurface();
        const EglApi &egl = EglApi::get();
        if (context && egl.ok)
            egl.destroyContext(display, context);
        context = nullptr;
        program = vao = 0;
        shareContext = nullptr;
    }

    unsigned compile(Enum type, const char *body)
    {
        const QByteArray prefix = es ? QByteArrayLiteral("#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n")
                                     : QByteArrayLiteral("#version 330 core\n");
        const QByteArray source = prefix + body;
        const char *text = source.constData();
        const unsigned shader = gl.createShader(type);
        gl.shaderSource(shader, 1, &text, nullptr);
        gl.compileShader(shader);
        int ok = 0;
        gl.getShaderiv(shader, 0x8B81 /* COMPILE_STATUS */, &ok);
        if (!ok) {
            gl.deleteShader(shader);
            return 0;
        }
        return shader;
    }

    // The pass: the two eyes of the source, cropped, side by side in the window surface (row 0 of the texture is the top of the picture)
    bool buildProgram()
    {
        static const char vertex[] = "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
        static const char fragment[] =
                "uniform sampler2D u_src;\nuniform ivec2 u_crop;\nuniform ivec2 u_eye;\nuniform int u_swap;\nuniform int u_width;\nout vec4 o_color;\n"
                "void main()\n{\n    ivec2 p = ivec2(gl_FragCoord.xy);\n    int e = p.x >= u_eye.x ? 1 : 0;\n    int se = u_swap != 0 ? 1 - e : e;\n"
                "    ivec2 s = ivec2(se * u_width + u_crop.x + (p.x - e * u_eye.x), u_crop.y + (u_eye.y - 1 - p.y));\n"
                "    o_color = vec4(texelFetch(u_src, s, 0).rgb, 1.0);\n}\n";
        const unsigned vs = compile(0x8B31 /* VERTEX_SHADER */, vertex);
        const unsigned fs = vs ? compile(0x8B30 /* FRAGMENT_SHADER */, fragment) : 0;
        if (!vs || !fs)
            return false;
        program = gl.createProgram();
        gl.attachShader(program, vs);
        gl.attachShader(program, fs);
        gl.linkProgram(program);
        gl.deleteShader(vs);
        gl.deleteShader(fs);
        int ok = 0;
        gl.getProgramiv(program, 0x8B82 /* LINK_STATUS */, &ok);
        if (!ok) {
            program = 0;
            return false;
        }
        gl.genVertexArrays(1, &vao);
        uSrc = gl.getUniformLocation(program, "u_src");
        uCrop = gl.getUniformLocation(program, "u_crop");
        uEye = gl.getUniformLocation(program, "u_eye");
        uSwap = gl.getUniformLocation(program, "u_swap");
        uWidth = gl.getUniformLocation(program, "u_width");
        return true;
    }
};

struct QQuickVideoStereoLayer::Private
{
    QMutex mutex;
    QWindow *window = nullptr;
    wl_display *display = nullptr;
    wl_event_queue *queue = nullptr;
    wl_registry *registry = nullptr;
    Globals globals;
    wl_surface *surface = nullptr;
    wl_subsurface *subsurface = nullptr;
    wp_viewport *viewport = nullptr;
    wp_color_management_surface_v1 *colorSurface = nullptr;
    Buffer buffers[3];
    bool declared = false;
    bool stereoInitialized = false;
    QRect rect;
    QRect placed;
    QSize placedBuffer;
    bool mapped = false;
    bool colorSet = false;
    Color color;
    GpuSurface gpu;
    int frames = 0, shmDumped = 0;
    BufferFormat gpuWanted = BufferFormat::Xrgb8888;

    void place();
    void dispatch() { wl_display_dispatch_queue_pending(display, queue); }
};

namespace {
// what the compositor's color manager takes, read with the globals
Globals &compositorGlobals()
{
    static Globals g;
    return g;
}

// the description of the color of a video, as wp_color_manager_v1 names it; false for the
// one the compositor takes a surface for without description (SDR, BT.709 primaries)
bool describeColor(const QQuickVideoStereoLayer::Color &color, uint32_t *primaries, uint32_t *transfer)
{
    bool describe = false;
    *primaries = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
    if (color.space == QVideoFrameFormat::ColorSpace_BT2020) {
        *primaries = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
        describe = true;
    }
    switch (color.transfer) {
    case QVideoFrameFormat::ColorTransfer_ST2084:
        *transfer = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
        return true;
    case QVideoFrameFormat::ColorTransfer_STD_B67:
        *transfer = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG;
        return true;
    case QVideoFrameFormat::ColorTransfer_Linear:
        *transfer = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
        return true;
    default:
        // SDR: the transfer function of the compositor's default
        *transfer = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
        return describe;
    }
}
}

bool QQuickVideoStereoLayer::canDescribe(const Color &color)
{
    uint32_t primaries, transfer;
    if (!describeColor(color, &primaries, &transfer))
        return true;
    const Globals &g = compositorGlobals();
    return g.parametric && g.namedPrimaries.contains(primaries) && g.namedTransfers.contains(transfer);
}

bool QQuickVideoStereoLayer::isSupported()
{
    static const bool supported = [] {
        wl_display *display = waylandDisplay();
        if (!display)
            return false;
        wl_event_queue *queue = wl_display_create_queue(display);
        Globals globals;
        wl_registry *registry = readGlobals(display, queue, globals);
        const bool ok = globals.compositor && globals.subcompositor && globals.shm
                && globals.viewporter && globals.stereo;
        compositorGlobals().parametric = globals.parametric && globals.colorManager;
        compositorGlobals().namedPrimaries = globals.namedPrimaries;
        compositorGlobals().namedTransfers = globals.namedTransfers;
        destroyGlobals(globals);
        wl_registry_destroy(registry);
        wl_event_queue_destroy(queue);
        return ok;
    }();
    return supported;
}

QQuickVideoStereoLayer::QQuickVideoStereoLayer() : d(std::make_unique<Private>()) { }

std::unique_ptr<QQuickVideoStereoLayer> QQuickVideoStereoLayer::create(QWindow *window)
{
    if (!window || !isSupported())
        return nullptr;
    auto *windowInterface = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!windowInterface || !windowInterface->surface())
        return nullptr;

    std::unique_ptr<QQuickVideoStereoLayer> layer(new QQuickVideoStereoLayer);
    Private *p = layer->d.get();
    p->window = window;
    p->display = waylandDisplay();
    p->queue = wl_display_create_queue(p->display);
    p->registry = readGlobals(p->display, p->queue, p->globals);
    if (stereo_declare_wayland_init(p->display, p->queue) != 0)
        return nullptr;
    p->stereoInitialized = true;
    wl_display_roundtrip_queue(p->display, p->queue);
    if (!stereo_supported_wayland())
        return nullptr;

    p->surface = wl_compositor_create_surface(p->globals.compositor);
    p->subsurface = wl_subcompositor_get_subsurface(p->globals.subcompositor, p->surface,
                                                    windowInterface->surface());
    wl_subsurface_place_below(p->subsurface, windowInterface->surface());
    // The layer changes with the frames of the video, not with those of the window
    wl_subsurface_set_desync(p->subsurface);
    p->viewport = wp_viewporter_get_viewport(p->globals.viewporter, p->surface);
    if (p->globals.colorManager && p->globals.parametric)
        p->colorSurface = wp_color_manager_v1_get_surface(p->globals.colorManager, p->surface);

    // The window's surface keeps the pointer: the layer takes no input
    wl_region *empty = wl_compositor_create_region(p->globals.compositor);
    wl_surface_set_input_region(p->surface, empty);
    wl_region_destroy(empty);
    return layer;
}

QQuickVideoStereoLayer::~QQuickVideoStereoLayer()
{
    QMutexLocker lock(&d->mutex);
    d->gpu.dropAll();
    if (d->declared)
        stereo_remove_wayland(d->surface);
    if (d->stereoInitialized)
        stereo_declare_wayland_finish();
    if (d->colorSurface)
        wp_color_management_surface_v1_destroy(d->colorSurface);
    if (d->viewport)
        wp_viewport_destroy(d->viewport);
    if (d->subsurface)
        wl_subsurface_destroy(d->subsurface);
    if (d->surface)
        wl_surface_destroy(d->surface);
    for (Buffer &b : d->buffers)
        destroyBuffer(b);
    destroyGlobals(d->globals);
    if (d->registry)
        wl_registry_destroy(d->registry);
    if (d->queue)
        wl_event_queue_destroy(d->queue);
    if (d->display)
        wl_display_flush(d->display);
}

BufferFormat QQuickVideoStereoLayer::chooseFormat(BufferFormat wanted) const
{
    // the format asked for, else the next one down that the compositor offers: half float, 16
    // bits, 10 bits, 8 bits (XRGB8888 is always there)
    auto offered = [this](BufferFormat f) { return d->globals.shmFormats.contains(shmCode(f)); };
    const BufferFormat chain[] = { BufferFormat::Xbgr16161616f, BufferFormat::Xbgr16161616,
                                   BufferFormat::Xrgb2101010, BufferFormat::Xrgb8888 };
    bool reached = false;
    for (BufferFormat f : chain) {
        reached = reached || f == wanted;
        if (reached && (f == BufferFormat::Xrgb8888 || offered(f)))
            return f;
    }
    return BufferFormat::Xrgb8888;
}

bool QQuickVideoStereoLayer::hasColorManagement() const
{
    return d->colorSurface != nullptr;
}

void QQuickVideoStereoLayer::Private::place()
{
    if (rect.isEmpty())
        return;
    if (rect.topLeft() != placed.topLeft())
        wl_subsurface_set_position(subsurface, rect.x(), rect.y());
    if (rect.size() != placed.size()) {
        wp_viewport_set_destination(viewport, rect.width(), rect.height());
        wl_region *opaque = wl_compositor_create_region(globals.compositor);
        wl_region_add(opaque, 0, 0, rect.width(), rect.height());
        wl_surface_set_opaque_region(surface, opaque);
        wl_region_destroy(opaque);
    }
    placed = rect;
}

void QQuickVideoStereoLayer::setColor(const Color &color)
{
    QMutexLocker lock(&d->mutex);
    if (d->colorSet && d->color.space == color.space && d->color.transfer == color.transfer
        && d->color.maxLuminance == color.maxLuminance)
        return;
    d->color = color;
    d->colorSet = true;
    if (!d->colorSurface)
        return;

    // SDR video in the primaries of BT.709 is what the compositor takes a surface for when
    // it has no description; nothing is said for it, so that it looks as it does without the layer
    uint32_t primaries = 0;
    uint32_t transfer = 0;
    const bool describe = describeColor(color, &primaries, &transfer);
    if (!describe || !d->globals.namedPrimaries.contains(primaries) || !d->globals.namedTransfers.contains(transfer)) {
        if (describe)
            qCWarning(qLcStereoLayer) << "the compositor does not take this color description";
        wp_color_management_surface_v1_unset_image_description(d->colorSurface);
        return;
    }
    // made through a wrapper on the layer's queue, so that its events are not Qt's
    auto *manager = static_cast<wp_color_manager_v1 *>(wl_proxy_create_wrapper(d->globals.colorManager));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(manager), d->queue);
    wp_image_description_creator_params_v1 *creator = wp_color_manager_v1_create_parametric_creator(manager);
    wl_proxy_wrapper_destroy(manager);
    wp_image_description_creator_params_v1_set_primaries_named(creator, primaries);
    wp_image_description_creator_params_v1_set_tf_named(creator, transfer);
    // the peak of the display the video was mastered on, for the compositor's tone mapping
    if ((transfer == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
         || transfer == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG)
        && d->globals.masteringVolume && color.maxLuminance > 1 && color.maxLuminance <= 10000) {
        wp_image_description_creator_params_v1_set_mastering_luminance(creator, 0, uint32_t(color.maxLuminance));
    }
    wp_image_description_v1 *description = wp_image_description_creator_params_v1_create(creator);
    int state = 0;
    wp_image_description_v1_add_listener(description, &imageDescriptionListener, &state);
    while (state == 0 && wl_display_roundtrip_queue(d->display, d->queue) >= 0) { }
    if (state > 0) {
        wp_color_management_surface_v1_set_image_description(d->colorSurface, description,
                                                             WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        qCDebug(qLcStereoLayer) << "color description set: primaries" << primaries << "transfer" << transfer;
    }
    wp_image_description_v1_destroy(description);
}

bool QQuickVideoStereoLayer::present(const QImage &left, const QImage &right, BufferFormat format,
                                    const QRect &rect)
{
    if (left.isNull() || left.size() != right.size())
        return false;
    QMutexLocker lock(&d->mutex);
    d->rect = rect;
    if (d->rect.isEmpty())
        return false;
    // a window surface of the GPU owns the buffers of the wl_surface: it is let go before wl_shm buffers are attached
    if (d->gpu.active()) {
        qCInfo(qLcStereoLayer) << "the views are shown through wl_shm buffers again: the window surface of the GPU is let go";
        d->gpu.dropSurface();
    }
    // Nothing is listened to on the queue of the layer but the release of buffers
    d->dispatch();

    const QSize pixels(left.width() * 2, left.height());
    Buffer *buffer = nullptr;
    for (Buffer &candidate : d->buffers) {
        if (candidate.buffer && (candidate.pixels != pixels || candidate.format != shmCode(format)) && !candidate.busy)
            destroyBuffer(candidate);
        if (candidate.busy)
            continue;
        if (!candidate.buffer && !createBuffer(candidate, d->globals.shm, d->queue, pixels, format))
            continue;
        buffer = &candidate;
        break;
    }
    if (!buffer)
        return false; // the compositor holds them all: retry after release, including paused updates

    writePair(left, right, format, buffer->data, pixels.width() * bytesPerPixel(format));
    // the diagnostic of the proofs, as for the GPU path: the buffer as the compositor gets it
    static const QByteArray dumpDir = qgetenv("QT_MM_STEREO_LAYER_DUMP");
    if (!dumpDir.isEmpty() && ++d->frames > 3 && d->shmDumped < 6) {
        const QString base = QString::fromLocal8Bit(dumpDir) + QStringLiteral("/shm-%1").arg(d->shmDumped++);
        QFile out(base + QStringLiteral(".raw"));
        if (out.open(QIODevice::WriteOnly))
            out.write(reinterpret_cast<const char *>(buffer->data), qint64(pixels.width()) * bytesPerPixel(format) * pixels.height());
        else
            qCWarning(qLcStereoLayer) << "the dump could not be written:" << out.errorString();
        QFile info(base + QStringLiteral(".txt"));
        if (info.open(QIODevice::WriteOnly))
            info.write(QStringLiteral("%1 %2 %3 rows from the top\n").arg(format == BufferFormat::Xrgb2101010 ? u"xr30"_s : format == BufferFormat::Xrgb8888 ? u"xr24"_s : u"xb4h"_s).arg(pixels.width()).arg(pixels.height()).toUtf8());
    }

    d->place();
    if (!d->declared) {
        const int result = stereo_declare_wayland(d->surface, STEREO_SBS_FULL, STEREO_CLASS_VIDEO, STEREO_VIDEO_CURRENT);
        if (result < 0)
            qCWarning(qLcStereoLayer) << "declaring the stereo content failed:" << strerror(-result);
        else
            d->declared = true;
    }
    buffer->busy = true;
    wl_surface_attach(d->surface, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(d->surface, 0, 0, pixels.width(), pixels.height());
    wl_surface_commit(d->surface);
    wl_display_flush(d->display);
    d->mapped = true;
    return true;
}

bool QQuickVideoStereoLayer::gpuActive() const
{
    QMutexLocker lock(&d->mutex);
    return d->gpu.active();
}

bool QQuickVideoStereoLayer::compositorHasDmabuf() const
{
    return d->globals.dmabuf;
}

bool QQuickVideoStereoLayer::presentGpu(const QQuickVideoDepth::LayerSource &source, bool swapEyes, const QRect &crop,
                                        BufferFormat format, const QRect &rect, BufferFormat *used, QString *why)
{
    const EglApi &egl = EglApi::get();
    if (!egl.ok) {
        *why = QStringLiteral("libEGL or libwayland-egl could not be loaded");
        return false;
    }
    if (!source.eglDisplay || !source.eglContext || !source.texture || crop.isEmpty() || rect.isEmpty()
        || crop.x() < 0 || crop.y() < 0 || crop.x() + crop.width() > source.width / 2 || crop.y() + crop.height() > source.height) {
        *why = QStringLiteral("the views of the frame cannot be drawn by the layer (no shared context, or an empty or outside crop)");
        return false;
    }
    QMutexLocker lock(&d->mutex);
    GpuSurface &g = d->gpu;
    d->rect = rect;
    d->dispatch();
    const QSize pixels(crop.width() * 2, crop.height());
    const bool es = source.es;
    // the formats from the one asked for down, as chooseFormat() does for wl_shm
    const BufferFormat chain[] = { BufferFormat::Xrgb2101010, BufferFormat::Xbgr16161616f, BufferFormat::Xbgr16161616, BufferFormat::Xrgb8888 };
    if (g.context && (g.display != source.eglDisplay || g.shareContext != source.eglContext || g.es != es || d->gpuWanted != format)) {
        qCInfo(qLcStereoLayer) << "the layer's context is made again:" << (g.display != source.eglDisplay) << (g.shareContext != source.eglContext)
                               << (g.es != es) << (d->gpuWanted != format);
        g.dropAll();
    }
    if (g.active() && g.size != pixels) {
        egl.windowResize(g.window, pixels.width(), pixels.height(), 0, 0);
        g.size = pixels;
    }
    if (!g.active()) {
        g.dropSurface();
        egl.bindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API);
        QString lastWhy;
        // Only the format asked for: a surface of less depth than the views have is no hand-over (the wl_shm buffers of the presenter carry
        // the depth the compositor offers), so the caller shows them through present() and says why
        // (a surface of more depth is no loss: 10 bit content goes into a half float surface where the driver has no 10 bit one)
        for (BufferFormat f : chain) {
            if (f != format && !(format == BufferFormat::Xrgb2101010 && f == BufferFormat::Xbgr16161616f))
                continue;
            EGLint attributes[32];
            if (!configAttributes(f, es, attributes, 32)) {
                lastWhy = QStringLiteral("EGL has no window surface of 16 bit unsigned normalised");
                continue;
            }
            EGLConfig config = nullptr;
            EGLint count = 0;
            if (!egl.chooseConfig(source.eglDisplay, attributes, &config, 1, &count) || count < 1) {
                lastWhy = QStringLiteral("the driver has no window surface of this format (it has %1)").arg(windowFormatsOf(source.eglDisplay));
                continue;
            }
            if (g.context && g.format != f)
                g.dropAll();
            if (!g.context) {
                const EGLint contextAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, es ? 0 : 3,
                                                     EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
                const EGLint esAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
                g.context = egl.createContext(source.eglDisplay, config, source.eglContext, es ? esAttributes : contextAttributes);
                if (!g.context) {
                    lastWhy = QStringLiteral("a context sharing the drawing context's could not be made (EGL error 0x%1)").arg(egl.getError(), 0, 16);
                    continue;
                }
                g.display = source.eglDisplay;
                g.shareContext = source.eglContext;
                g.es = es;
            }
            g.window = egl.windowCreate(d->surface, pixels.width(), pixels.height());
            g.surface = g.window ? egl.createWindowSurface(source.eglDisplay, config, reinterpret_cast<EGLNativeWindowType>(g.window), nullptr)
                                 : nullptr;
            if (!g.surface) {
                lastWhy = QStringLiteral("the driver made no window surface of this format for the layer (EGL error 0x%1)").arg(egl.getError(), 0, 16);
                g.dropSurface();
                continue;
            }
            g.config = config;
            g.format = f;
            g.size = pixels;
            d->gpuWanted = format;
            break;
        }
        if (!g.active()) {
            *why = lastWhy.isEmpty() ? QStringLiteral("no window surface could be made for the layer") : lastWhy;
            return false;
        }
        qCInfo(qLcStereoLayer) << "the views are drawn into a window surface of the GPU, shared with the drawing context: format" << int(g.format) << ":"
                               << (d->globals.dmabuf ? "the compositor offers zwp_linux_dmabuf_v1" : "the compositor does not offer zwp_linux_dmabuf_v1");
    }
    *used = g.format;
    if (!egl.makeCurrent(g.display, g.surface, g.surface, g.context)) {
        *why = QStringLiteral("the layer's context could not be made current (EGL error 0x%1)").arg(egl.getError(), 0, 16);
        g.dropSurface();
        return false;
    }
    g.current = true;
    if (!g.program) {
        if (!g.gl.load(egl) || !g.buildProgram()) {
            *why = QStringLiteral("the layer's pass does not build on this OpenGL");
            g.dropAll();
            return false;
        }
        egl.swapInterval(g.display, 0);
    }
    g.gl.viewport(0, 0, pixels.width(), pixels.height());
    g.gl.disable(0x0BE2 /* BLEND */);
    g.gl.disable(0x0C11 /* SCISSOR_TEST */);
    g.gl.useProgram(g.program);
    g.gl.bindVertexArray(g.vao);
    g.gl.activeTexture(0x84C0);
    g.gl.bindTexture(0x0DE1 /* TEXTURE_2D */, source.texture);
    g.gl.uniform1i(g.uSrc, 0);
    g.gl.uniform2i(g.uCrop, crop.x(), crop.y());
    g.gl.uniform2i(g.uEye, crop.width(), crop.height());
    g.gl.uniform1i(g.uSwap, swapEyes ? 1 : 0);
    g.gl.uniform1i(g.uWidth, source.width / 2);
    g.gl.drawArrays(0x0004 /* TRIANGLES */, 0, 3);

    // A diagnostic for the proofs: the first frames of the surface read back as the surface holds them (RGBA8, RGB10_A2, RGBA half float), into
    // QT_MM_STEREO_LAYER_DUMP
    static const QByteArray dumpDir = qgetenv("QT_MM_STEREO_LAYER_DUMP");
    if (!dumpDir.isEmpty() && ++d->frames > 3 && g.dumped < 6) {
        const bool tenBit = g.format == BufferFormat::Xrgb2101010, half = g.format == BufferFormat::Xbgr16161616f;
        const int bytes = half ? 8 : 4;
        QByteArray pixelsBack(pixels.width() * pixels.height() * bytes, 0);
        g.gl.readPixels(0, 0, pixels.width(), pixels.height(), 0x1908 /* RGBA */, half ? 0x140B /* HALF_FLOAT */ : tenBit ? 0x8368 /* UNSIGNED_INT_2_10_10_10_REV */ : 0x1401, pixelsBack.data());
        const QString base = QString::fromLocal8Bit(dumpDir) + QStringLiteral("/layer-%1").arg(g.dumped++);
        QFile out(base + QStringLiteral(".raw"));
        if (out.open(QIODevice::WriteOnly))
            out.write(pixelsBack);
        QFile info(base + QStringLiteral(".txt"));
        if (info.open(QIODevice::WriteOnly))
            info.write(QStringLiteral("%1 %2 %3 rows from the bottom\n").arg(half ? u"rgba16f"_s : tenBit ? u"rgb10a2"_s : u"rgba8"_s).arg(pixels.width()).arg(pixels.height()).toUtf8());
    }

    d->place();
    if (!d->declared) {
        const int result = stereo_declare_wayland(d->surface, STEREO_SBS_FULL, STEREO_CLASS_VIDEO, STEREO_VIDEO_CURRENT);
        if (result < 0)
            qCWarning(qLcStereoLayer) << "declaring the stereo content failed:" << strerror(-result);
        else
            d->declared = true;
    }
    const bool swapped = egl.swapBuffers(g.display, g.surface);
    // the textures are the drawing context's: finished here before the frame is let go and they are drawn into again
    g.gl.finish();
    egl.makeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    g.current = false;
    if (!swapped) {
        *why = QStringLiteral("the window surface of the layer could not be swapped (EGL error 0x%1)").arg(egl.getError(), 0, 16);
        g.dropSurface();
        return false;
    }
    wl_display_flush(d->display);
    d->mapped = true;
    return true;
}

void QQuickVideoStereoLayer::hide()
{
    QMutexLocker lock(&d->mutex);
    if (!d->mapped)
        return;
    // a window surface of the GPU is kept: the driver attaches its next buffer at the next swap
    wl_surface_attach(d->surface, nullptr, 0, 0);
    wl_surface_commit(d->surface);
    wl_display_flush(d->display);
    d->mapped = false;
}

QT_END_NAMESPACE

#else // no Wayland client library, or no stereo content declaration to build against

QT_BEGIN_NAMESPACE

struct QQuickVideoStereoLayer::Private { };

bool QQuickVideoStereoLayer::isSupported() { return false; }
std::unique_ptr<QQuickVideoStereoLayer> QQuickVideoStereoLayer::create(QWindow *) { return nullptr; }
QQuickVideoStereoLayer::QQuickVideoStereoLayer() = default;
QQuickVideoStereoLayer::~QQuickVideoStereoLayer() = default;
QQuickVideoStereo::BufferFormat QQuickVideoStereoLayer::chooseFormat(QQuickVideoStereo::BufferFormat) const
{
    return QQuickVideoStereo::BufferFormat::Xrgb8888;
}
bool QQuickVideoStereoLayer::present(const QImage &, const QImage &, QQuickVideoStereo::BufferFormat,
                                    const QRect &) { return false; }
void QQuickVideoStereoLayer::hide() { }
void QQuickVideoStereoLayer::setColor(const Color &) { }
bool QQuickVideoStereoLayer::hasColorManagement() const { return false; }
bool QQuickVideoStereoLayer::canDescribe(const Color &) { return false; }
bool QQuickVideoStereoLayer::gpuActive() const { return false; }
bool QQuickVideoStereoLayer::compositorHasDmabuf() const { return false; }
bool QQuickVideoStereoLayer::presentGpu(const QQuickVideoDepth::LayerSource &, bool, const QRect &, QQuickVideoStereo::BufferFormat,
                                        const QRect &, QQuickVideoStereo::BufferFormat *, QString *why)
{
    *why = QStringLiteral("no stereo layer");
    return false;
}

QT_END_NAMESPACE

#endif
