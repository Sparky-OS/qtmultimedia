// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qquickvideostereolayer_p.h"

#ifdef QT_MM_STEREO_LAYER

#include <QtCore/qloggingcategory.h>
#include <QtCore/qmutex.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qwindow.h>
#include <QtGui/qpa/qplatformwindow_p.h>

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <stereo-declare.h>

#include "wayland-viewporter-client-protocol.h"
#include "wayland-color-management-v1-client-protocol.h"

QT_BEGIN_NAMESPACE

Q_STATIC_LOGGING_CATEGORY(qLcStereoLayer, "qt.multimedia.video.stereolayer")

using namespace QQuickVideoStereo;

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

void QQuickVideoStereoLayer::setRect(const QRect &rect)
{
    QMutexLocker lock(&d->mutex);
    if (d->rect == rect)
        return;
    d->rect = rect;
    if (d->mapped) {
        d->place();
        wl_surface_commit(d->surface);
        wl_display_flush(d->display);
    }
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

void QQuickVideoStereoLayer::present(const QImage &left, const QImage &right, BufferFormat format)
{
    if (left.isNull() || left.size() != right.size())
        return;
    QMutexLocker lock(&d->mutex);
    if (d->rect.isEmpty())
        return;
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
        return; // the compositor holds them all: this frame is dropped

    writePair(left, right, format, buffer->data, pixels.width() * bytesPerPixel(format));

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
}

void QQuickVideoStereoLayer::hide()
{
    QMutexLocker lock(&d->mutex);
    if (!d->mapped)
        return;
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
void QQuickVideoStereoLayer::setRect(const QRect &) { }
void QQuickVideoStereoLayer::present(const QImage &, const QImage &, QQuickVideoStereo::BufferFormat) { }
void QQuickVideoStereoLayer::hide() { }
void QQuickVideoStereoLayer::setColor(const Color &) { }
bool QQuickVideoStereoLayer::hasColorManagement() const { return false; }
bool QQuickVideoStereoLayer::canDescribe(const Color &) { return false; }

QT_END_NAMESPACE

#endif
