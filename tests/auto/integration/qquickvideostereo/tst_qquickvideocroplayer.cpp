// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QtTest/qtest.h>
#include <QtCore/qfile.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qsemaphore.h>
#include <QtMultimedia/qabstractvideobuffer.h>
#include <QtQuick/qquickwindow.h>
#include <QtMultimedia/qvideosink.h>
#include <QtMultimediaQuick/private/qquickvideooutput_p.h>

class tst_QQuickVideoCropLayer : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cropResizeAndClear();
    void callerParAndMultiview();
    void busyPausedResize();
    void ordinary2D();
    void convertingArrivalsAndInvalidation();
};

static QVideoFrame frame(QVideoFrameFormat::StereoMode mode, bool second = false,
                         bool multiview = false, QSize par = QSize(1, 1),
                         bool rightFirst = false, bool hlg = false, int sourceOffset = 0)
{
    const bool packed = multiview || mode == QVideoFrameFormat::StereoMode::SideBySide;
    QVideoFrameFormat format(QSize(packed ? 320 : 160, 90), QVideoFrameFormat::Format_BGRA8888);
    format.setStereoMode(mode);
    format.setPixelAspectRatio(par);
    QVideoFrameFormat::StereoFlags flags;
    if (multiview)
        flags |= QVideoFrameFormat::StereoFlag::Multiview;
    if (rightFirst)
        flags |= QVideoFrameFormat::StereoFlag::RightEyeFirst;
    format.setStereoFlags(flags);
    if (mode == QVideoFrameFormat::StereoMode::FrameSequence)
        format.setStereoView(second ? QVideoFrameFormat::StereoView::Right : QVideoFrameFormat::StereoView::Left);
    format.setColorSpace(QVideoFrameFormat::ColorSpace_BT709);
    format.setColorTransfer(hlg ? QVideoFrameFormat::ColorTransfer_STD_B67
                                : QVideoFrameFormat::ColorTransfer_Gamma22);
    format.setColorRange(QVideoFrameFormat::ColorRange_Full);
    QVideoFrame result(format);
    if (!result.map(QVideoFrame::WriteOnly))
        return {};
    for (int y = 0; y < 90; ++y) {
        auto *row = result.bits(0) + y * result.bytesPerLine(0);
        for (int x = 0; x < format.frameWidth(); ++x) {
            row[4 * x] = (packed ? (x < 160 ? 64 : 192) : (second ? 208 : 80)) + sourceOffset;
            row[4 * x + 1] = y;
            row[4 * x + 2] = x % 160;
            row[4 * x + 3] = 255;
        }
    }
    result.unmap();
    result.setStartTime(1000 + sourceOffset * 1000);
    return result;
}

// Optional observing adapter boundary: the proof rig records actual declared
// SHM commits, not item properties. No recorder is injected into production.
// The qualified native invocation sets QT_STEREO_TEST_BUFFER_DIR and requires
// these complete sample/count checks, including the absence of stale commits.
static bool submittedBuffer(int count, int width, int destination, int leftBlue, int rightBlue)
{
    const QString directory = qEnvironmentVariable("QT_STEREO_TEST_BUFFER_DIR");
    if (directory.isEmpty())
        return false;
    QFile log(directory + u"/log.jsonl");
    if (!log.open(QIODevice::ReadOnly))
        return false;
    const auto lines = log.readAll().trimmed().split('\n');
    if (lines.size() != count) {
        qWarning() << "Submitted buffers" << lines.size() << "expected" << count;
        return false;
    }
    const auto entry = QJsonDocument::fromJson(lines.last()).object();
    const auto target = entry.value("destination").toArray();
    if (entry.value("width").toInt() != width || entry.value("height").toInt() != 90
            || entry.value("format").toInt() != 1 || entry.value("content").toInt() != 3
            || target.size() != 2 || target[0].toInt() != destination || target[1].toInt() != 120)
        return false;
    QFile buffer(directory + u'/' + entry.value("file").toString());
    if (!buffer.open(QIODevice::ReadOnly))
        return false;
    const auto bytes = buffer.readAll();
    const int stride = entry.value("stride").toInt();
    if (stride < width * 4 || bytes.size() != stride * 90)
        return false;
    const int viewWidth = width / 2;
    const int x0 = (160 - viewWidth) / 2;
    for (int y = 0; y < 90; ++y) {
        for (int eye = 0; eye < 2; ++eye) {
            for (int x = 0; x < viewWidth; ++x) {
                const auto *pixel = reinterpret_cast<const uchar *>(bytes.constData())
                        + y * stride + (eye * viewWidth + x) * 4;
                if (pixel[0] != (eye ? rightBlue : leftBlue)
                        || pixel[1] != y || pixel[2] != x0 + x)
                    return false;
            }
        }
    }
    return true;
}

void tst_QQuickVideoCropLayer::initTestCase()
{
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (maps.open(QIODevice::ReadOnly)) {
        for (const auto &line : maps.readAll().split('\n')) {
            if (line.contains("libQt6Multimedia") && line.contains("r-xp"))
                qInfo().noquote() << line;
        }
    }
}

void tst_QQuickVideoCropLayer::cropResizeAndClear()
{
    if (QGuiApplication::platformName() != u"wayland")
        QSKIP("Requires an isolated Wayland stereo compositor");
    if (!qEnvironmentVariableIsSet("QT_STEREO_TEST_BUFFER_DIR"))
        QSKIP("Requires an observing adapter for native submitted buffers");
    QQuickWindow window;
    auto format = window.format();
    format.setAlphaBufferSize(8);
    window.setFormat(format);
    window.resize(160, 120);
    QQuickVideoOutput output;
    output.setParentItem(window.contentItem());
    output.setSize(QSizeF(160, 120));
    output.setFillMode(QQuickVideoOutput::PreserveAspectCrop);
    window.show();
    if (!output.stereoLayerSupported())
        QSKIP("The compositor does not expose the stereo layer contract");
    QVERIFY(window.format().hasAlpha());
    qInfo("CROP_PHASE packed-160x120");
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide));
    QTRY_VERIFY_WITH_TIMEOUT(output.stereoVideo(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(output.stereoLayerActive(), 3000);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(1, 240, 160, 64, 192));
    qInfo("CROP_PHASE packed-120x120");
    output.setWidth(120);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(2, 180, 120, 64, 192));
    qInfo("CROP_PHASE temporal-120x120");
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::FrameSequence));
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::FrameSequence, true));
    QTest::qWait(250);
    QVERIFY(submittedBuffer(3, 180, 120, 80, 208));
    qInfo("CROP_PHASE paused-temporal-160x120");
    output.setWidth(160); // no new decoded temporal frame
    QTest::qWait(250);
    output.setVisible(false);
    QTest::qWait(150);
    output.setVisible(true);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(5, 240, 160, 80, 208));
    qInfo("CROP_PHASE transparent-parent");
    window.contentItem()->setOpacity(0);
    QTest::qWait(150);
    window.contentItem()->setOpacity(1);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(6, 240, 160, 80, 208));
    qInfo("CROP_PHASE partial-opacity-restore");
    window.contentItem()->setOpacity(0.5);
    QTest::qWait(250);
    QVERIFY(!output.stereoLayerActive());
    window.contentItem()->setOpacity(1);
    QTest::qWait(250);
    QVERIFY(output.stereoLayerActive());
    QVERIFY(submittedBuffer(7, 240, 160, 80, 208));
    qInfo("CROP_PHASE transformed-restore");
    output.setRotation(10);
    QTest::qWait(250);
    QVERIFY(!output.stereoLayerActive());
    output.setRotation(0);
    QTest::qWait(250);
    QVERIFY(output.stereoLayerActive());
    QVERIFY(submittedBuffer(8, 240, 160, 80, 208));
    qInfo("CROP_PHASE clipped-restore");
    window.contentItem()->setClip(true);
    output.setX(1);
    QTest::qWait(250);
    QVERIFY(!output.stereoLayerActive());
    output.setX(0);
    QTest::qWait(250);
    QVERIFY(output.stereoLayerActive());
    QVERIFY(submittedBuffer(9, 240, 160, 80, 208));
    qInfo("CROP_PHASE single-eye-restore");
    output.setStereoView(QQuickVideoOutput::StereoLeftEye);
    QTest::qWait(250);
    QVERIFY(!output.stereoLayerActive());
    output.setStereoView(QQuickVideoOutput::StereoBothEyes);
    QTest::qWait(250);
    QVERIFY(output.stereoLayerActive());
    QVERIFY(submittedBuffer(10, 240, 160, 80, 208));
    qInfo("CROP_PHASE paused-eye-swap");
    output.setStereoSwapEyes(true);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(11, 240, 160, 208, 80));
    output.setStereoSwapEyes(false);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(12, 240, 160, 80, 208));
    qInfo("CROP_PHASE presenter-recreated");
    output.setParentItem(nullptr);
    QTest::qWait(150);
    output.setParentItem(window.contentItem());
    QTest::qWait(350);
    QVERIFY(output.stereoLayerActive());
    QVERIFY(submittedBuffer(13, 240, 160, 80, 208));
    qInfo("CROP_PHASE source-clear-incomplete");
    output.clearOutput();
    QTest::qWait(150);
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::FrameSequence,
                                          false, false, QSize(1, 1), false, false, 16));
    QTest::qWait(250);
    QVERIFY(!output.stereoLayerActive());
    QVERIFY(submittedBuffer(13, 240, 160, 80, 208));
    qInfo("CROP_PHASE new-source-pair");
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::FrameSequence,
                                          true, false, QSize(1, 1), false, false, 16));
    QTest::qWait(250);
    QVERIFY(output.stereoLayerActive());
    QVERIFY(submittedBuffer(14, 240, 160, 96, 224));
    qInfo("CROP_PHASE ordinary-2D");
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::None));
    QTRY_VERIFY_WITH_TIMEOUT(!output.stereoVideo(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!output.stereoLayerActive(), 3000);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(14, 240, 160, 96, 224));
    qInfo("CROP_PHASE explicit-single-eye-fallback");
    output.setStereoView(QQuickVideoOutput::StereoLeftEye);
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide));
    QTRY_VERIFY_WITH_TIMEOUT(output.stereoVideo(), 3000);
    QVERIFY(!output.stereoLayerActive());
    QTest::qWait(150);
    output.videoSink()->setVideoFrame(QVideoFrame());
    QVERIFY(!output.stereoLayerActive());
}

void tst_QQuickVideoCropLayer::callerParAndMultiview()
{
    if (QGuiApplication::platformName() != u"wayland")
        QSKIP("Requires an isolated Wayland stereo compositor");
    if (!qEnvironmentVariableIsSet("QT_STEREO_TEST_BUFFER_DIR"))
        QSKIP("Requires an observing adapter for native submitted buffers");
    QQuickWindow window;
    window.resize(160, 120);
    QQuickVideoOutput output;
    output.setParentItem(window.contentItem());
    output.setSize(QSizeF(160, 120));
    output.setFillMode(QQuickVideoOutput::PreserveAspectCrop);
    window.show();
    if (!output.stereoLayerSupported())
        QSKIP("No stereo layer contract");
    qInfo("CROP_PHASE actual-caller-PAR");
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide,
                                          false, false, QSize(2, 1)));
    QTest::qWait(350);
    QVERIFY(output.stereoLayerActive());
    QVERIFY(submittedBuffer(1, 120, 160, 64, 192));
    qInfo("CROP_PHASE actual-caller-PAR-swapped");
    output.setStereoSwapEyes(true);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(2, 120, 160, 192, 64));
    qInfo("CROP_PHASE actual-caller-MV-right-first");
    output.setStereoSwapEyes(false);
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide,
                                          false, true, QSize(2, 1), true));
    QTest::qWait(350);
    QVERIFY(output.stereoLayerActive());
    QVERIFY(submittedBuffer(3, 120, 160, 192, 64));
    qInfo("CROP_PHASE actual-caller-MV-swapped");
    output.setStereoSwapEyes(true);
    QTest::qWait(250);
    QVERIFY(submittedBuffer(4, 120, 160, 64, 192));
    qInfo("CROP_PHASE colour-fallback");
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide,
                                          false, false, QSize(1, 1), false, true));
    QTest::qWait(250);
    // The GPU-free QPainter fixture has no HLG colour-description contract.
    if (qEnvironmentVariableIsSet("CROP_REQUIRE_COLOUR_FALLBACK"))
        QVERIFY(!output.stereoLayerActive());
    QVERIFY(submittedBuffer(4, 120, 160, 64, 192));
    qInfo("CROP_PHASE colour-fallback-restore-SDR");
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide));
    QTest::qWait(300);
    QVERIFY(output.stereoLayerActive());
    output.videoSink()->setVideoFrame(QVideoFrame());
    QTest::qWait(150);
    QVERIFY(!output.stereoLayerActive());
}

void tst_QQuickVideoCropLayer::busyPausedResize()
{
    if (QGuiApplication::platformName() != u"wayland"
            || !qEnvironmentVariableIsSet("CROP_RELEASE_GATE"))
        QSKIP("Requires the isolated delayed-wl_buffer-release test adapter");
    QQuickWindow window;
    window.resize(160, 120);
    QQuickVideoOutput output;
    output.setParentItem(window.contentItem());
    output.setSize(QSizeF(160, 120));
    output.setFillMode(QQuickVideoOutput::PreserveAspectCrop);
    window.show();
    QVERIFY(output.stereoLayerSupported());
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide));
    QTest::qWait(300);
    output.setWidth(150);
    QTest::qWait(150);
    output.setWidth(140);
    QTest::qWait(150);
    qInfo("CROP_PHASE busy-paused-resize-120");
    output.setWidth(120); // all three buffers remain held by the test adapter
    QTest::qWait(200);
    QFile gate(qEnvironmentVariable("CROP_RELEASE_GATE"));
    QVERIFY(submittedBuffer(3, 210, 140, 64, 192));
    QVERIFY(gate.open(QIODevice::WriteOnly));
    gate.close(); // release buffers without submitting another decoded frame
    QTest::qWait(500);
    QVERIFY(submittedBuffer(4, 180, 120, 64, 192));
    QVERIFY(output.stereoLayerActive());
}

void tst_QQuickVideoCropLayer::ordinary2D()
{
    if (QGuiApplication::platformName() != u"wayland"
            && QGuiApplication::platformName() != u"xcb")
        QSKIP("Requires an isolated Wayland or X11 compositor");
    QQuickWindow window;
    window.resize(160, 120);
    window.setColor(Qt::black);
    QQuickVideoOutput output;
    output.setParentItem(window.contentItem());
    output.setSize(QSizeF(160, 120));
    output.setFillMode(QQuickVideoOutput::PreserveAspectCrop);
    window.show();
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::None));
    QTest::qWait(350);
    QVERIFY(!output.stereoVideo());
    QVERIFY(!output.stereoLayerActive());
    QImage image = window.grabWindow();
    QVERIFY(!image.isNull());
    // A blank software-backend frame is not a successful video control.
    QVERIFY(image.pixelColor(80, 60) != QColor(Qt::black));
    QVERIFY(image.pixelColor(10, 10) != image.pixelColor(140, 100));
    if (qEnvironmentVariableIsSet("CROP_OUT"))
        QVERIFY(image.save(qEnvironmentVariable("CROP_OUT") + u"/ordinary2D.png"));
}

namespace {
struct ConversionGate { QSemaphore entered, resume; };
class DelayedBuffer : public QAbstractVideoBuffer
{
public:
    DelayedBuffer(QVideoFrame value, std::shared_ptr<ConversionGate> gate)
        : m_frame(std::move(value)), m_gate(std::move(gate)) { }
    QVideoFrameFormat format() const override { return m_frame.surfaceFormat(); }
    MapData map(QVideoFrame::MapMode mode) override
    {
        if (!m_entered) {
            m_entered = true;
            m_gate->entered.release();
            if (!m_gate->resume.tryAcquire(1, 3000))
                return {};
        }
        if (!m_frame.map(mode))
            return {};
        MapData data;
        data.planeCount = m_frame.planeCount();
        for (int i = 0; i < data.planeCount; ++i) {
            data.bytesPerLine[i] = m_frame.bytesPerLine(i);
            data.data[i] = m_frame.bits(i);
            data.dataSize[i] = m_frame.mappedBytes(i);
        }
        return data;
    }
    void unmap() override { m_frame.unmap(); }
private:
    QVideoFrame m_frame;
    std::shared_ptr<ConversionGate> m_gate;
    bool m_entered = false;
};
}

void tst_QQuickVideoCropLayer::convertingArrivalsAndInvalidation()
{
    if (QGuiApplication::platformName() != u"wayland")
        QSKIP("Requires an isolated Wayland stereo compositor");
    if (!qEnvironmentVariableIsSet("QT_STEREO_TEST_BUFFER_DIR"))
        QSKIP("Requires an observing adapter for native submitted buffers");
    QQuickWindow window;
    auto format = window.format();
    format.setAlphaBufferSize(8);
    window.setFormat(format);
    window.resize(160, 120);
    window.show();
    QTest::qWait(200);
    QQuickVideoOutput output;
    output.setParentItem(window.contentItem());
    output.setSize(QSizeF(160, 120));
    output.setFillMode(QQuickVideoOutput::PreserveAspectCrop);
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide));
    QTest::qWait(300);
    QVERIFY(output.stereoLayerActive());
    auto delayed = [&](std::shared_ptr<ConversionGate> gate) {
        auto value = frame(QVideoFrameFormat::StereoMode::SideBySide);
        output.videoSink()->setVideoFrame(
                QVideoFrame(std::make_unique<DelayedBuffer>(value, gate)));
    };
    for (int i = 0; i < 4; ++i) {
        qInfo("CROP_PHASE converting-incoming-%d", i);
        auto gate = std::make_shared<ConversionGate>();
        delayed(gate);
        QVERIFY(gate->entered.tryAcquire(1, 1000));
        // Every blocked conversion overlaps a real incoming replacement.
        output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide));
        gate->resume.release();
        QTest::qWait(150);
        QVERIFY(submittedBuffer(3 + 2 * i, 240, 160, 64, 192));
    }
    qInfo("CROP_PHASE resize-during-conversion");
    auto resize = std::make_shared<ConversionGate>();
    delayed(resize);
    QVERIFY(resize->entered.tryAcquire(1, 1000));
    output.setWidth(120);
    QTest::qWait(150);
    resize->resume.release();
    QTest::qWait(200);
    QVERIFY(submittedBuffer(10, 180, 120, 64, 192));
    qInfo("CROP_PHASE clear-during-conversion");
    auto clear = std::make_shared<ConversionGate>();
    delayed(clear);
    QVERIFY(clear->entered.tryAcquire(1, 1000));
    output.clearOutput();
    clear->resume.release();
    QTest::qWait(200); // must commit no old pair
    QVERIFY(submittedBuffer(10, 180, 120, 64, 192));
    qInfo("CROP_PHASE new-source-after-clear");
    output.setWidth(160);
    output.videoSink()->setVideoFrame(frame(QVideoFrameFormat::StereoMode::SideBySide));
    QTest::qWait(200);
}

QTEST_MAIN(tst_QQuickVideoCropLayer)
#include "tst_qquickvideocroplayer.moc"
