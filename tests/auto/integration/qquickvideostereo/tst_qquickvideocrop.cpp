// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QtTest/qtest.h>
#include <QtQuick/qquickitem.h>
#include <QtQuick/qquickwindow.h>
#include <QtMultimediaQuick/private/qquickvideostereo_p.h>
#ifndef QT_STEREO_CROP_BEFORE
#include <QtMultimediaQuick/private/qquickvideostereopresenter_p.h>
#endif

using namespace QQuickVideoStereo;
using Mode = QVideoFrameFormat::StereoMode;

class tst_QQuickVideoCrop : public QObject
{
    Q_OBJECT
private slots:
    void nativeCrop_data();
    void nativeCrop();
#ifndef QT_STEREO_CROP_BEFORE
    void roundingAndDisplayShape();
    void pausedPairResizeAndLifecycle();
    void cropFallback();
    void continuousArrivals();
    void busyRetryState();
#endif
};

void tst_QQuickVideoCrop::nativeCrop_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("full");
    QTest::addColumn<QSize>("frameSize");
    QTest::addColumn<QSize>("storedSize");
    QTest::addColumn<QRect>("expectedCrop");
    QTest::newRow("half SBS") << int(Mode::SideBySide) << false << QSize(32, 18) << QSize(16, 18) << QRect(2, 0, 12, 18);
    QTest::newRow("half TAB") << int(Mode::TopBottom) << false << QSize(32, 18) << QSize(32, 9) << QRect(4, 0, 24, 9);
    QTest::newRow("quincunx") << int(Mode::SideBySideQuincunx) << false << QSize(32, 18) << QSize(16, 18) << QRect(2, 0, 12, 18);
    QTest::newRow("rows") << int(Mode::RowInterleaved) << false << QSize(32, 18) << QSize(32, 9) << QRect(4, 0, 24, 9);
    QTest::newRow("columns") << int(Mode::ColumnInterleaved) << false << QSize(32, 18) << QSize(16, 18) << QRect(2, 0, 12, 18);
    QTest::newRow("checkerboard") << int(Mode::Checkerboard) << false << QSize(32, 18) << QSize(16, 18) << QRect(2, 0, 12, 18);
    QTest::newRow("full SBS") << int(Mode::SideBySide) << true << QSize(64, 18) << QSize(32, 18) << QRect(4, 0, 24, 18);
    QTest::newRow("full TAB") << int(Mode::TopBottom) << true << QSize(32, 36) << QSize(32, 18) << QRect(4, 0, 24, 18);
    QTest::newRow("sequence") << int(Mode::FrameSequence) << false << QSize(32, 18) << QSize(32, 18) << QRect(4, 0, 24, 18);
}

void tst_QQuickVideoCrop::nativeCrop()
{
    QFETCH(int, mode);
    QFETCH(bool, full);
    QFETCH(QSize, frameSize);
    QFETCH(QSize, storedSize);
    QFETCH(QRect, expectedCrop);
    QImage frame(frameSize, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < frame.height(); ++y) {
        auto *line = reinterpret_cast<float *>(frame.scanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            line[4 * x] = x;
            line[4 * x + 1] = y;
            line[4 * x + 2] = 0;
            line[4 * x + 3] = 1;
        }
    }
    Packing packing;
    packing.mode = Mode(mode);
    packing.size = full ? Packing::Size::Full : Packing::Size::Half;
    for (int eye : { 0, 1 }) {
        QImage view = storedView(frame, packing, eye);
        QCOMPARE(view.size(), storedSize);
#ifndef QT_STEREO_CROP_BEFORE
        const QRect crop = coverCrop(view.size(), QSize(32, 18), QSize(4, 3));
        QCOMPARE(crop, expectedCrop);
        view = view.copy(crop);
#endif
        QCOMPARE(view.size(), expectedCrop.size());
        for (int y = 0; y < view.height(); ++y) {
            const auto *line = reinterpret_cast<const float *>(view.constScanLine(y));
            for (int x = 0; x < view.width(); ++x) {
                int sx = x + expectedCrop.x(), sy = y + expectedCrop.y();
                switch (packing.mode) {
                case Mode::SideBySide:
                case Mode::SideBySideQuincunx: sx += eye * frameSize.width() / 2; break;
                case Mode::TopBottom: sy += eye * frameSize.height() / 2; break;
                case Mode::RowInterleaved: sy = 2 * sy + eye; break;
                case Mode::ColumnInterleaved: sx = 2 * sx + eye; break;
                case Mode::Checkerboard: sx = 2 * sx + (sy + eye) % 2; break;
                default: break;
                }
                QCOMPARE(line[4 * x], float(sx));
                QCOMPARE(line[4 * x + 1], float(sy));
            }
        }
    }
}

#ifndef QT_STEREO_CROP_BEFORE
void tst_QQuickVideoCrop::roundingAndDisplayShape()
{
    QCOMPARE(coverCrop(QSize(9, 7), QSize(16, 9), QSize(4, 3)), QRect(1, 0, 7, 7));
    QCOMPARE(coverCrop(QSize(16, 9), QSize(4, 3), QSize(16, 9)), QRect(0, 1, 16, 7));
    QCOMPARE(coverCrop(QSize(960, 1080), QSize(1920, 1080), QSize(1440, 1080)), QRect(120, 0, 720, 1080));
    QCOMPARE(coverCrop(QSize(1920, 540), QSize(1920, 1080), QSize(1440, 1080)), QRect(240, 0, 1440, 540));
    QVERIFY(coverCrop({}, QSize(16, 9), QSize(4, 3)).isEmpty());
    QVERIFY(coverCrop(QSize(16, 9), {}, QSize(4, 3)).isEmpty());
    QVERIFY(coverCrop(QSize(16, 9), QSize(16, 9), {}).isEmpty());
}

void tst_QQuickVideoCrop::pausedPairResizeAndLifecycle()
{
    using Presenter = QQuickVideoStereoPresenter;
    Presenter::State state;
    state.setGeometry({ QRect(10, 20, 160, 90), true });
    Presenter::Job job;
    job.frame = QVideoFrame(QImage(32, 18, QImage::Format_RGBA8888));
    job.otherFrame = QVideoFrame(QImage(32, 18, QImage::Format_RGBA8888));
    job.frame.setStartTime(1000);
    job.otherFrame.setStartTime(1001);
    job.frameView = 1;
    job.packing.mode = Mode::FrameSequence;
    job.displaySize = QSize(32, 18);
    state.submit(job);
    const auto old = state.take();
    QVERIFY(old.visible);
    // No new decoded frame: resize must retain the already resolved pair/order.
    state.setGeometry({ QRect(10, 20, 120, 90), true });
    QVERIFY(!state.isCurrent(old.revision));
    const auto resized = state.take();
    QCOMPARE(resized.job.frame.startTime(), qint64(1000));
    QCOMPARE(resized.job.otherFrame.startTime(), qint64(1001));
    QCOMPARE(resized.job.frameView, 1);
    QCOMPARE(resized.geometry.destination.size(), QSize(120, 90));
    state.hide();
    QVERIFY(!state.isCurrent(resized.revision));
    QVERIFY(!state.take().visible);
    state.setGeometry(resized.geometry);
    QVERIFY(state.take().visible);
    // An incomplete temporal pair must not replace the last complete pair.
    job.otherFrame = {};
    state.submit(job);
    QCOMPARE(state.take().job.otherFrame.startTime(), qint64(1001));
    state.clear();
    state.setGeometry(resized.geometry);
    QVERIFY(!state.take().visible);
    // A fresh packed source cannot inherit an old temporal eye or crop flag.
    job.packing.mode = Mode::SideBySide;
    state.setGeometry({ QRect(0, 0, 160, 90), false });
    state.submit(job);
    const auto fresh = state.take();
    QVERIFY(fresh.visible);
    QVERIFY(!fresh.geometry.crop);
    QVERIFY(!fresh.job.otherFrame.isValid());
}

void tst_QQuickVideoCrop::continuousArrivals()
{
    using Presenter = QQuickVideoStereoPresenter;
    Presenter::State state;
    const Presenter::Geometry geometry{ QRect(0, 0, 160, 120), true };
    state.setGeometry(geometry);
    Presenter::Job job;
    job.frame = QVideoFrame(QImage(320, 90, QImage::Format_RGBA8888));
    job.packing.mode = Mode::SideBySide;
    state.submit(job);
    int published = 0;
    qint64 last = -1;
    for (int i = 0; i < 100; ++i) {
        const auto converting = state.take();
        // A decoder arrival in the unlocked conversion interval must not
        // invalidate a complete, still-current pair. The next take uses it.
        job.frame.setStartTime(i + 1);
        state.submit(job);
        QTest::qSleep(1);
        if (state.isCurrent(converting.revision)) {
            QVERIFY(converting.job.frame.startTime() >= last);
            last = converting.job.frame.startTime();
            ++published;
        }
    }
    QCOMPARE(published, 100);
    const auto resizing = state.take();
    state.setGeometry({ QRect(0, 0, 120, 120), true });
    QVERIFY(!state.isCurrent(resizing.revision));
    const auto hiding = state.take();
    state.hide();
    QVERIFY(!state.isCurrent(hiding.revision));
    state.setGeometry(geometry);
    const auto clearing = state.take();
    state.clear();
    QVERIFY(!state.isCurrent(clearing.revision));
    state.setGeometry(geometry);
    QVERIFY(!state.take().visible);
}

void tst_QQuickVideoCrop::busyRetryState()
{
    using Presenter = QQuickVideoStereoPresenter;
    Presenter::State state;
    const Presenter::Geometry geometry{ QRect(0, 0, 160, 120), true };
    state.setGeometry(geometry);
    Presenter::Job job;
    job.frame = QVideoFrame(QImage(320, 90, QImage::Format_RGBA8888));
    job.packing.mode = Mode::SideBySide;
    state.submit(job);
    const auto busy = state.take();
    state.retryPresentation(busy.revision);
    QVERIFY(state.dirty);
    QVERIFY(state.retry);
    const auto retry = state.take();
    QCOMPARE(retry.geometry.destination, geometry.destination);
    QVERIFY(retry.job.frame.isValid());
    state.clear();
    state.retryPresentation(retry.revision);
    QVERIFY(!state.retry);
    state.setGeometry(geometry);
    QVERIFY(!state.take().visible);
    job.sourceSerial = 1;
    state.submit(job);
    const auto oldSource = state.take();
    job.sourceSerial = 2;
    state.submit(job);
    QVERIFY(!state.isCurrent(oldSource.revision));
    const auto corrected = state.take();
    QCOMPARE(corrected.job.sourceSerial, quint64(2));
    job.swapEyes = true;
    state.submit(job);
    QVERIFY(!state.isCurrent(corrected.revision));
    QVERIFY(state.take().job.swapEyes);
}

void tst_QQuickVideoCrop::cropFallback()
{
    QQuickItem parent;
    parent.setSize(QSizeF(100, 100));
    QQuickItem item(&parent);
    item.setSize(QSizeF(80, 80));
    item.setPosition(QPointF(0.4, 0.4));
    QVERIFY(!canCropLayer(&item));
    item.setPosition(QPointF(0, 0));
    QVERIFY(canCropLayer(&item));
    item.setSize(QSizeF(80.4, 60.4));
    QVERIFY(!canCropLayer(&item));
    item.setSize(QSizeF(80, 80));
    parent.setClip(true);
    QVERIFY(canCropLayer(&item));
    item.setX(30);
    QVERIFY(!canCropLayer(&item));
    item.setX(0);
    parent.setRotation(20);
    QVERIFY(!canCropLayer(&item));
    parent.setRotation(0);
    parent.setOpacity(0.5);
    QVERIFY(!canCropLayer(&item));
    parent.setOpacity(1);
    QVERIFY(canCropLayer(&item));
    QQuickWindow window;
    window.resize(100, 100);
    qInfo() << "Logical placement checks at DPR" << window.devicePixelRatio();
    window.contentItem()->setSize(QSizeF(100, 100));
    QQuickItem edge;
    edge.setParentItem(window.contentItem());
    edge.setSize(QSizeF(80, 80));
    QVERIFY(canCropLayer(&edge));
    edge.setPosition(QPointF(20, 20));
    QVERIFY(canCropLayer(&edge));
    edge.setX(20.01);
    QVERIFY(!canCropLayer(&edge));
    edge.setX(30);
    QVERIFY(!canCropLayer(&edge));
}
#endif

QTEST_MAIN(tst_QQuickVideoCrop)
#include "tst_qquickvideocrop.moc"
