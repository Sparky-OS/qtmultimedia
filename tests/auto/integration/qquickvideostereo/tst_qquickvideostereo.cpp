// Copyright (C) 2026 Daniel Campos Ramos
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QtTest/qtest.h>
#include <QtTest/qsignalspy.h>

#include <QtCore/qobject.h>
#include <QtGui/qimage.h>
#include <QtMultimedia/qvideoframe.h>
#include <QtMultimedia/qvideoframeformat.h>
#include <QtMultimedia/qvideosink.h>
#include <QtMultimedia/private/qvideoframeconverter_p.h>
#include <QtMultimediaQuick/private/qquickvideooutput_p.h>
#include <QtMultimediaQuick/private/qquickvideostereo_p.h>

#include <set>

using namespace QQuickVideoStereo;
using Mode = QVideoFrameFormat::StereoMode;

namespace {

// A frame whose pixel (x, y) is (x, y) in the red and green channels, in float
QImage coordinateImage(int w, int h)
{
    QImage image(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<float *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            line[x * 4] = float(x);
            line[x * 4 + 1] = float(y);
            line[x * 4 + 2] = 0.f;
            line[x * 4 + 3] = 1.f;
        }
    }
    return image;
}

QPointF at(const QImage &image, int x, int y)
{
    const auto *p = reinterpret_cast<const float *>(image.constScanLine(y)) + x * 4;
    return QPointF(p[0], p[1]);
}

}

class tst_QQuickVideoStereo : public QObject
{
    Q_OBJECT

private slots:
    void sizes_data();
    void sizes();
    void unpackExact_data();
    void unpackExact();
    void upscale();
    void levels10Bit();
    void deepConversion();
    void propertiesDefault();
    void stereoVideoFromFrames();
};

void tst_QQuickVideoStereo::sizes_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("size");
    QTest::addColumn<QSize>("frame");
    QTest::addColumn<QSize>("view");
    using S = Packing::Size;
    QTest::newRow("sbs half by shape") << int(Mode::SideBySide) << int(S::ByShape) << QSize(1920, 1080) << QSize(1920, 1080);
    QTest::newRow("sbs full by shape (32:9)") << int(Mode::SideBySide) << int(S::ByShape) << QSize(3840, 1080) << QSize(1920, 1080);
    QTest::newRow("sbs full forced") << int(Mode::SideBySide) << int(S::Full) << QSize(2560, 1440) << QSize(1280, 1440);
    QTest::newRow("tb half by shape") << int(Mode::TopBottom) << int(S::ByShape) << QSize(1920, 1080) << QSize(1920, 1080);
    QTest::newRow("tb full by shape") << int(Mode::TopBottom) << int(S::ByShape) << QSize(1920, 2160) << QSize(1920, 1080);
    QTest::newRow("rows") << int(Mode::RowInterleaved) << int(S::ByShape) << QSize(1920, 1080) << QSize(1920, 1080);
    QTest::newRow("columns") << int(Mode::ColumnInterleaved) << int(S::ByShape) << QSize(1920, 1080) << QSize(1920, 1080);
    QTest::newRow("checkerboard") << int(Mode::Checkerboard) << int(S::ByShape) << QSize(1920, 1080) << QSize(1920, 1080);
}

void tst_QQuickVideoStereo::sizes()
{
    QFETCH(int, mode);
    QFETCH(int, size);
    QFETCH(QSize, frame);
    QFETCH(QSize, view);
    Packing packing;
    packing.mode = Mode(mode);
    packing.size = Packing::Size(size);
    QCOMPARE(viewSize(packing, frame), view);
}

void tst_QQuickVideoStereo::unpackExact_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("size");
    // the expected source pixel of view `index` at its (1, 1)
    QTest::addColumn<int>("index");
    QTest::addColumn<QPointF>("expected");
    using S = Packing::Size;
    // a 16x8 frame; full size views where they are not interleaved or squeezed
    QTest::newRow("sbs full, first") << int(Mode::SideBySide) << int(S::Full) << 0 << QPointF(1, 1);
    QTest::newRow("sbs full, second") << int(Mode::SideBySide) << int(S::Full) << 1 << QPointF(9, 1);
    QTest::newRow("tb full, first") << int(Mode::TopBottom) << int(S::Full) << 0 << QPointF(1, 1);
    QTest::newRow("tb full, second") << int(Mode::TopBottom) << int(S::Full) << 1 << QPointF(1, 5);
}

void tst_QQuickVideoStereo::unpackExact()
{
    QFETCH(int, mode);
    QFETCH(int, size);
    QFETCH(int, index);
    QFETCH(QPointF, expected);
    Packing packing;
    packing.mode = Mode(mode);
    packing.size = Packing::Size(size);
    const QImage frame = coordinateImage(16, 8);
    const QImage view = extractView(frame, packing, index, Scaling::Lanczos);
    QCOMPARE(view.size(), viewSize(packing, frame.size()));
    // unscaled views are the pixels of the frame, exactly
    QCOMPARE(at(view, 1, 1), expected);
}

void tst_QQuickVideoStereo::upscale()
{
    // a half side by side frame of a horizontal ramp: each view is the ramp squeezed to half the
    // width; restored to full width it is the ramp again, with Lanczos within a small error, and the
    // interleaved ones the same at their own offsets
    const int w = 64, h = 4;
    QImage frame(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<float *>(frame.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float v = float((x % (w / 2)) * 2 + 1) / float(w); // each half: the ramp over the full width
            line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = v;
            line[x * 4 + 3] = 1.f;
        }
    }
    Packing packing;
    packing.mode = Mode::SideBySide;
    packing.size = Packing::Size::Half;
    for (Scaling scaling : { Scaling::Bilinear, Scaling::Bicubic, Scaling::Lanczos }) {
        const QImage view = extractView(frame, packing, 0, scaling);
        QCOMPARE(view.size(), QSize(w, h));
        double worst = 0;
        // away from the edges, where the ramp is clamped
        for (int x = 8; x < w - 8; ++x)
            worst = std::max(worst, std::abs(double(at(view, x, 1).x()) - double(x + 0.5) / w));
        QVERIFY2(worst < 0.02, qPrintable(QString::number(worst)));
    }

    // columns: the first view has the even columns of a ramp, restored it is the ramp
    QImage ramp(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<float *>(ramp.scanLine(y));
        for (int x = 0; x < w; ++x) {
            line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = float(x + 0.5) / w;
            line[x * 4 + 3] = 1.f;
        }
    }
    packing.mode = Mode::ColumnInterleaved;
    for (int index : { 0, 1 }) {
        const QImage view = extractView(ramp, packing, index, Scaling::Lanczos);
        QCOMPARE(view.size(), QSize(w, h));
        double worst = 0;
        for (int x = 8; x < w - 8; ++x)
            worst = std::max(worst, std::abs(double(at(view, x, 1).x()) - double(x + 0.5) / w));
        QVERIFY2(worst < 0.02, qPrintable(QString::number(worst)));
    }
}

void tst_QQuickVideoStereo::levels10Bit()
{
    // a 10 bit ramp keeps its levels in the buffer of the layer: more than 256 in each view
    const int w = 2048, h = 2;
    QImage left(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<float *>(left.scanLine(y));
        for (int x = 0; x < w; ++x) {
            line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = float(x) / (w - 1);
            line[x * 4 + 3] = 1.f;
        }
    }
    for (BufferFormat format : { BufferFormat::Xrgb8888, BufferFormat::Xrgb2101010 }) {
        QByteArray buffer(w * 2 * h * bytesPerPixel(format), 0);
        writePair(left, left, format, reinterpret_cast<uchar *>(buffer.data()), w * 2 * bytesPerPixel(format));
        std::set<quint32> levels;
        for (int x = 0; x < w * 2; ++x) {
            const quint32 pixel = reinterpret_cast<const quint32 *>(buffer.constData())[x];
            levels.insert(format == BufferFormat::Xrgb8888 ? (pixel >> 16) & 0xff : (pixel >> 20) & 0x3ff);
        }
        if (format == BufferFormat::Xrgb8888)
            QCOMPARE(levels.size(), size_t(256));
        else
            QCOMPARE(levels.size(), size_t(1024));
    }
}

void tst_QQuickVideoStereo::deepConversion()
{
    // a 10 bit video, converted by the GPU or by the CPU where there is none: the floating point
    // image holds more than 256 levels, the 8 bit one cannot
    const QSize size(1024, 2);
    QVideoFrameFormat format(size, QVideoFrameFormat::Format_YUV420P10);
    format.setColorSpace(QVideoFrameFormat::ColorSpace_BT709);
    format.setColorRange(QVideoFrameFormat::ColorRange_Full);
    QVideoFrame frame(format);
    QVERIFY(frame.map(QVideoFrame::WriteOnly));
    for (int y = 0; y < size.height(); ++y) {
        auto *line = reinterpret_cast<quint16 *>(frame.bits(0) + y * frame.bytesPerLine(0));
        for (int x = 0; x < size.width(); ++x)
            line[x] = quint16(x); // 10 bit, one level per column
    }
    for (int plane : { 1, 2 }) {
        for (int y = 0; y < size.height() / 2; ++y) {
            auto *line = reinterpret_cast<quint16 *>(frame.bits(plane) + y * frame.bytesPerLine(plane));
            for (int x = 0; x < size.width() / 2; ++x)
                line[x] = 512;
        }
    }
    frame.unmap();

    const QImage deep = qImageFromVideoFrame(frame, QVideoFrameImageDepth::Float16);
    if (deep.format() != QImage::Format_RGBA16FPx4_Premultiplied)
        QSKIP("no GPU rendering to floating point here");
    std::set<float> levels;
    for (int x = 0; x < deep.width(); ++x)
        levels.insert(deep.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied).pixelColor(x, 0).redF());
    QVERIFY2(levels.size() > 800, qPrintable(QString::number(levels.size())));
    const QImage plain = qImageFromVideoFrame(frame, QVideoFrameImageDepth::Bits8);
    std::set<int> eight;
    for (int x = 0; x < plain.width(); ++x)
        eight.insert(qRed(plain.pixel(x, 0)));
    QVERIFY(eight.size() <= 256);
}

void tst_QQuickVideoStereo::propertiesDefault()
{
    QQuickVideoOutput output;
    QCOMPARE(output.stereoInput(), QQuickVideoOutput::StereoAutomatic);
    QCOMPARE(output.stereoSwapEyes(), false);
    QCOMPARE(output.stereoView(), QQuickVideoOutput::StereoBothEyes);
    QCOMPARE(output.stereoShapeFallback(), true); // the edition takes a wide undeclared video for side by side
    QCOMPARE(output.stereoScaling(), QQuickVideoOutput::StereoLanczos);
    QCOMPARE(output.stereoSurface(), QQuickVideoOutput::StereoSurfaceBySource);
    QCOMPARE(output.stereoVideo(), false);
    QCOMPARE(output.stereoPacking(), QQuickVideoOutput::StereoNone);
    QCOMPARE(output.stereoLayerActive(), false);

    QSignalSpy input(&output, &QQuickVideoOutput::stereoInputChanged);
    output.setStereoInput(QQuickVideoOutput::StereoTopBottomHalf);
    output.setStereoInput(QQuickVideoOutput::StereoTopBottomHalf);
    QCOMPARE(input.size(), 1);
    QCOMPARE(output.stereoInput(), QQuickVideoOutput::StereoTopBottomHalf);
}

void tst_QQuickVideoStereo::stereoVideoFromFrames()
{
    QQuickVideoOutput output;
    QVideoSink *sink = output.videoSink();
    QSignalSpy video(&output, &QQuickVideoOutput::stereoVideoChanged);

    // a frame that says it is side by side, 3840x1080: two views of 1920x1080
    output.setStereoView(QQuickVideoOutput::StereoBothEyes);
    QVideoFrameFormat format(QSize(3840, 1080), QVideoFrameFormat::Format_ARGB8888_Premultiplied);
    format.setStereoMode(Mode::SideBySide);
    sink->setVideoFrame(QVideoFrame(format));
    QTRY_COMPARE(output.stereoVideo(), true);
    QCOMPARE(output.stereoPacking(), QQuickVideoOutput::StereoSideBySideFull);
    QTRY_COMPARE(output.implicitWidth(), 1920);
    QCOMPARE(output.implicitHeight(), 1080);

    // not 3D, as the application says
    output.setStereoInput(QQuickVideoOutput::StereoNone);
    QTRY_COMPARE(output.stereoVideo(), false);
    QTRY_COMPARE(output.implicitWidth(), 3840);

    // a frame that says nothing, as wide as two 16:9: taken for full side by side when asked
    output.setStereoInput(QQuickVideoOutput::StereoAutomatic);
    output.setStereoShapeFallback(false);
    sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(QSize(3840, 1080), QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
    QTRY_COMPARE(output.stereoVideo(), false);
    // 2.39:1 is not as wide as two pictures: not 3D with the fallback on either
    output.setStereoShapeFallback(true);
    sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(QSize(1912, 800), QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
    QTRY_COMPARE(output.stereoVideo(), false);
    sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(QSize(3840, 1080), QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
    QTRY_COMPARE(output.stereoVideo(), true);
    QCOMPARE(output.stereoPacking(), QQuickVideoOutput::StereoSideBySideFull);

    // and the other way: nothing but the first of two views is the right eye's with swapped eyes
    output.setStereoView(QQuickVideoOutput::StereoRightEye);
    QTRY_COMPARE(output.implicitWidth(), 1920);
}

QTEST_MAIN(tst_QQuickVideoStereo)
#include "tst_qquickvideostereo.moc"
