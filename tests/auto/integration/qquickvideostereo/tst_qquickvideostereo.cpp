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
#include <QtMultimediaQuick/private/qquickvideodepth_p.h>
#include <QtCore/qrandom.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qtransform.h>
#include <QtQuick/qquickwindow.h>

#include <algorithm>
#include <cmath>
#include <set>

using namespace QQuickVideoStereo;
using Mode = QVideoFrameFormat::StereoMode;
using namespace Qt::StringLiterals;

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
    void pairImageFormats_data();
    void pairImageFormats();
    void stereoVideoFromFrames();
    void depthLayouts_data();
    void depthLayouts();
    void depthMarks();
    void depthInVideoOutput();
    void depthRotated_data();
    void depthRotated();
    void depthRotatedFormats_data();
    void depthRotatedFormats();
    void depthRotatedInVideoOutput();
    void depthRotatedDrawn_data();
    void depthRotatedDrawn();
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
    // a single picture is never 3D by its shape: not a scope film (2.39:1, 2.40:1), not the widest
    // film formats (Ultra Panavision 70, 2.76:1), not a frame just under 3:1
    output.setStereoShapeFallback(true);
    for (const QSize size : { QSize(1912, 800), QSize(1920, 800), QSize(1920, 696), QSize(2999, 1000) }) {
        sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(QSize(3840, 1080), QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
        QTRY_COMPARE(output.stereoVideo(), true);
        sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(size, QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
        QTRY_VERIFY2(!output.stereoVideo(), qPrintable(QStringLiteral("%1x%2").arg(size.width()).arg(size.height())));
    }
    // two pictures next to each other are: full side by side 16:9 (3.56:1), and 3:1 itself
    for (const QSize size : { QSize(3840, 1080), QSize(1440, 480) }) {
        sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(QSize(1920, 1080), QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
        QTRY_COMPARE(output.stereoVideo(), false);
        sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(size, QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
        QTRY_VERIFY2(output.stereoVideo(), qPrintable(QStringLiteral("%1x%2").arg(size.width()).arg(size.height())));
        QCOMPARE(output.stereoPacking(), QQuickVideoOutput::StereoSideBySideFull);
    }
    // below 3:1 the application's choice makes it 3D
    output.setStereoInput(QQuickVideoOutput::StereoSideBySideFull);
    sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(QSize(1920, 800), QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
    QTRY_COMPARE(output.stereoVideo(), true);
    output.setStereoInput(QQuickVideoOutput::StereoAutomatic);
    sink->setVideoFrame(QVideoFrame(QVideoFrameFormat(QSize(3840, 1080), QVideoFrameFormat::Format_ARGB8888_Premultiplied)));
    QTRY_COMPARE(output.stereoVideo(), true);

    // and the other way: nothing but the first of two views is the right eye's with swapped eyes
    output.setStereoView(QQuickVideoOutput::StereoRightEye);
    QTRY_COMPARE(output.implicitWidth(), 1920);
}

void tst_QQuickVideoStereo::pairImageFormats_data()
{
    QTest::addColumn<int>("imageFormat");
    QTest::addColumn<int>("bufferFormat");
    for (int image : { int(QImage::Format_RGBA8888_Premultiplied),
                       int(QImage::Format_RGBA16FPx4_Premultiplied),
                       int(QImage::Format_RGBA32FPx4_Premultiplied) }) {
        for (int buffer = 0; buffer < 4; ++buffer)
            QTest::newRow(qPrintable(QString::number(image) + QLatin1Char('-') + QString::number(buffer))) << image << buffer;
    }
}

void tst_QQuickVideoStereo::pairImageFormats()
{
    QFETCH(int, imageFormat);
    QFETCH(int, bufferFormat);
    // Padding makes each row safe even for the former incorrect float32 cast.
    QByteArray leftStorage(8 * 8 * 16, '\0'), rightStorage(8 * 8 * 16, '\0');
    QImage left(reinterpret_cast<uchar *>(leftStorage.data()), 8, 8, 8 * 16, QImage::Format(imageFormat));
    QImage right(reinterpret_cast<uchar *>(rightStorage.data()), 8, 8, 8 * 16, QImage::Format(imageFormat));
    left.fill(QColor::fromRgbF(.75, .125, .25));
    right.fill(QColor::fromRgbF(.125, .25, .75));
    const auto format = BufferFormat(bufferFormat);
    const int stride = 16 * bytesPerPixel(format);
    QByteArray pixels(stride * 8, '\0');
    writePair(left, right, format, reinterpret_cast<uchar *>(pixels.data()), stride);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            const uchar *pixel = reinterpret_cast<const uchar *>(pixels.constData() + y * stride + x * bytesPerPixel(format));
            float red, blue;
            switch (format) {
            case BufferFormat::Xrgb8888: {
                const quint32 value = *reinterpret_cast<const quint32 *>(pixel);
                red = ((value >> 16) & 255) / 255.f; blue = (value & 255) / 255.f;
                break;
            }
            case BufferFormat::Xrgb2101010: {
                const quint32 value = *reinterpret_cast<const quint32 *>(pixel);
                red = ((value >> 20) & 1023) / 1023.f; blue = (value & 1023) / 1023.f;
                break;
            }
            case BufferFormat::Xbgr16161616: {
                const auto *value = reinterpret_cast<const quint16 *>(pixel);
                red = value[0] / 65535.f; blue = value[2] / 65535.f;
                break;
            }
            case BufferFormat::Xbgr16161616f: {
                const auto *value = reinterpret_cast<const qfloat16 *>(pixel);
                red = float(value[0]); blue = float(value[2]);
                break;
            }
            }
            QVERIFY2(x < 8 ? red > .7f && blue < .3f : blue > .7f && red < .3f,
                     qPrintable(QStringLiteral("wrong eye at %1,%2: %3,%4").arg(x).arg(y).arg(red).arg(blue)));
        }
    }
}

namespace {

// A frame of a depth layout, as a decoder gives it (YUV 4:4:4, full range): a picture of noise in
// the luma, its depth map a constant 64 with a box of 160 (larger nearer); the graphics and the
// background of their layouts, a band of depth 250 and a picture of depth 40.
QVideoFrame depthFrame(Mode mode, bool depthFirst, int w, int h)
{
    QRandomGenerator rng(151);
    QSize size(w, h);
    switch (mode) {
    case Mode::DepthSideBySide:
        size = QSize(2 * w, h);
        break;
    case Mode::DepthTopBottom:
        size = QSize(w, 2 * h);
        break;
    case Mode::DepthGraphics:
        size = QSize(w, 4 * h);
        break;
    case Mode::DepthBackground:
        size = QSize(2 * w, 2 * h);
        break;
    default:
        break;
    }
    QVideoFrameFormat format(size, QVideoFrameFormat::Format_YUV444P);
    format.setColorRange(QVideoFrameFormat::ColorRange_Full);
    format.setStereoMode(mode);
    if (depthFirst)
        format.setStereoFlags(QVideoFrameFormat::StereoFlag::DepthFirst);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly))
        return {};
    auto picture = [&](int x0, int y0) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                frame.bits(0)[(y0 + y) * frame.bytesPerLine(0) + x0 + x] =
                        uchar(rng.bounded(40, 216));
                frame.bits(1)[(y0 + y) * frame.bytesPerLine(1) + x0 + x] = 128;
                frame.bits(2)[(y0 + y) * frame.bytesPerLine(2) + x0 + x] = 128;
            }
    };
    auto depth = [&](int x0, int y0, auto value) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                frame.bits(0)[(y0 + y) * frame.bytesPerLine(0) + x0 + x] = uchar(value(x, y));
                frame.bits(1)[(y0 + y) * frame.bytesPerLine(1) + x0 + x] = 128;
                frame.bits(2)[(y0 + y) * frame.bytesPerLine(2) + x0 + x] = 128;
            }
    };
    auto boxDepth = [&](int x, int y) {
        return x >= w / 4 && x < w / 2 && y >= h / 4 && y < 3 * h / 4 ? 160 : 64;
    };
    switch (mode) {
    case Mode::DepthSideBySide:
        picture(depthFirst ? w : 0, 0);
        depth(depthFirst ? 0 : w, 0, boxDepth);
        break;
    case Mode::DepthTopBottom:
        picture(0, depthFirst ? h : 0);
        depth(0, depthFirst ? 0 : h, boxDepth);
        break;
    case Mode::DepthGraphics:
        picture(0, 0);
        depth(0, h, boxDepth);
        picture(0, 2 * h);
        depth(0, 3 * h, [&](int, int) { return 0; }); // no graphics
        break;
    case Mode::DepthBackground:
        picture(0, 0);
        depth(w, 0, boxDepth);
        picture(0, h);
        depth(w, h, [&](int, int) { return 40; });
        break;
    default:
        break;
    }
    frame.unmap();
    return frame;
}

// The shift of the luma of row y around x from the left view to the right one, by block matching
double shiftAt(const QVideoFrame &pair, int y, int x, int search)
{
    QVideoFrame f = pair;
    if (!f.map(QVideoFrame::ReadOnly))
        return qQNaN();
    const int w = f.width() / 2, half = 4;
    const uchar *luma = f.bits(0);
    const int stride = f.bytesPerLine(0);
    double best = 1e30;
    int bestDx = 0;
    for (int dx = -search; dx <= search; ++dx) {
        double cost = 0;
        for (int yy = y - half; yy <= y + half; ++yy)
            for (int xx = x - half; xx <= x + half; ++xx)
                cost += std::abs(int(luma[yy * stride + xx])
                                 - int(luma[yy * stride + w + xx + dx]));
        if (cost < best) {
            best = cost;
            bestDx = dx;
        }
    }
    f.unmap();
    return bestDx;
}

double expectedParallax(const QString &mark, int sample, int width)
{
    QQuickVideoDepth::Options o;
    Q_UNUSED(o);
    // nearness, the screen at 0.5 and 58 pixels at 1920 either side (libstereo-dibr)
    const double v = sample / 255.0;
    const double range = 58.0 * width / 1920.0;
    Q_UNUSED(mark);
    return v >= 0.5 ? -(v - 0.5) / 0.5 * range : (0.5 - v) / 0.5 * range;
}

} // namespace

void tst_QQuickVideoStereo::depthLayouts_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("depthFirst");
    QTest::newRow("side by side") << int(Mode::DepthSideBySide) << false;
    QTest::newRow("side by side, depth first") << int(Mode::DepthSideBySide) << true;
    QTest::newRow("top and bottom") << int(Mode::DepthTopBottom) << false;
    QTest::newRow("top and bottom, depth first") << int(Mode::DepthTopBottom) << true;
    QTest::newRow("graphics") << int(Mode::DepthGraphics) << false;
    QTest::newRow("background") << int(Mode::DepthBackground) << false;
}

void tst_QQuickVideoStereo::depthLayouts()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QFETCH(int, mode);
    QFETCH(bool, depthFirst);
    const int w = 240, h = 136;
    const QVideoFrame frame = depthFrame(Mode(mode), depthFirst, w, h);
    QVERIFY(frame.isValid());
    QQuickVideoDepth::Renderer renderer;
    QVideoFrame pair;
    QCOMPARE(renderer.draw(frame, Mode(mode), depthFirst, {}, {}, &pair),
             QQuickVideoDepth::Renderer::Result::Drawn);
    QCOMPARE(pair.size(), QSize(2 * w, h));
    QCOMPARE(pair.surfaceFormat().stereoMode(), Mode::SideBySide);
    QCOMPARE(pair.pixelFormat(), QVideoFrameFormat::Format_YUV444P);
    // the background (64) and the box (160) at their parallax, to the pixel
    QCOMPARE(shiftAt(pair, h / 8, w * 3 / 4, 12), qRound(expectedParallax({}, 64, w)));
    QCOMPARE(shiftAt(pair, h / 2, w * 3 / 8, 12), qRound(expectedParallax({}, 160, w)));
    QCOMPARE(QQuickVideoDepth::pictureRect(Mode(mode), depthFirst, frame.size()).size(),
             QSize(w, h));
}

void tst_QQuickVideoStereo::depthMarks()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    const int w = 240, h = 136;
    const QVideoFrame frame = depthFrame(Mode::DepthSideBySide, false, w, h);
    QQuickVideoDepth::Renderer renderer;
    QVideoFrame pair;
    // the stream's mark: larger samples farther, so the box is behind the background
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, u"farness"_s, {}, &pair),
             QQuickVideoDepth::Renderer::Result::Drawn);
    QCOMPARE(shiftAt(pair, h / 2, w * 3 / 8, 12), -qRound(expectedParallax({}, 160, w)));
    // the user's meaning comes before it, and the user's mark before both
    QQuickVideoDepth::Options o;
    o.meaning = 0;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, u"farness"_s, o, &pair),
             QQuickVideoDepth::Renderer::Result::Drawn);
    QCOMPARE(shiftAt(pair, h / 2, w * 3 / 8, 12), qRound(expectedParallax({}, 160, w)));
    o.mark = u"farness"_s;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &pair),
             QQuickVideoDepth::Renderer::Result::Drawn);
    QCOMPARE(shiftAt(pair, h / 2, w * 3 / 8, 12), -qRound(expectedParallax({}, 160, w)));
    // a malformed mark, the user's or the stream's, means one picture
    o.mark = u"nearness zero=abc"_s;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &pair),
             QQuickVideoDepth::Renderer::Result::PictureAlone);
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, u"nearness zero=abc"_s, {}, &pair),
             QQuickVideoDepth::Renderer::Result::PictureAlone);
}

void tst_QQuickVideoStereo::depthInVideoOutput()
{
    QQuickVideoOutput output;
    QVideoSink *sink = output.videoSink();
    const int w = 240, h = 136;
    sink->setVideoFrame(depthFrame(Mode::DepthSideBySide, false, w, h));
    QTRY_COMPARE(output.stereoPacking(), QQuickVideoOutput::StereoDepthSideBySide);
    QCOMPARE(output.stereoVideo(), true);
    // a view, or the picture alone: the size of the picture
    QTRY_COMPARE(output.implicitWidth(), qreal(w));
    output.setStereoDepth(false);
    QTRY_COMPARE(output.implicitWidth(), qreal(w));
    // the frame as stored
    output.setStereoView(QQuickVideoOutput::StereoFlat);
    QTRY_COMPARE(output.implicitWidth(), qreal(2 * w));
    // the user says how an unmarked frame packs its depth
    output.setStereoView(QQuickVideoOutput::StereoBothEyes);
    output.setStereoDepth(true);
    QVideoFrame unmarked = depthFrame(Mode::DepthTopBottom, false, w, h);
    QVideoFrameFormat format = unmarked.surfaceFormat();
    QCOMPARE(format.stereoMode(), Mode::DepthTopBottom);
    output.setStereoInput(QQuickVideoOutput::StereoDepthTopBottom);
    sink->setVideoFrame(unmarked);
    QTRY_COMPARE(output.stereoPacking(), QQuickVideoOutput::StereoDepthTopBottom);
    QTRY_COMPARE(output.implicitHeight(), qreal(h));
}

namespace {

// The luma of a region of a mapped planar frame
QImage lumaOf(const QVideoFrame &frame, QRect region)
{
    QVideoFrame f = frame;
    QImage image(region.size(), QImage::Format_Grayscale8);
    if (!f.map(QVideoFrame::ReadOnly))
        return {};
    for (int y = 0; y < region.height(); ++y)
        memcpy(image.scanLine(y), f.bits(0) + (region.y() + y) * f.bytesPerLine(0) + region.x(),
               region.width());
    f.unmap();
    return image;
}

// A picture turned as a frame asks for it: clockwise by the degrees, then mirrored horizontally
QImage turned(const QImage &gray, int degrees, bool mirrored)
{
    QImage result = degrees ? gray.transformed(QTransform().rotate(degrees)) : gray;
    return mirrored ? result.mirrored(true, false) : result;
}

// Where a pixel of the picture is after it is turned so
QPoint turnedPoint(QPoint p, QSize size, int degrees, bool mirrored)
{
    QPoint q = p;
    switch (degrees) {
    case 90:
        q = QPoint(size.height() - 1 - p.y(), p.x());
        break;
    case 180:
        q = QPoint(size.width() - 1 - p.x(), size.height() - 1 - p.y());
        break;
    case 270:
        q = QPoint(p.y(), size.width() - 1 - p.x());
        break;
    default:
        break;
    }
    if (mirrored)
        q.setX((degrees % 180 ? size.height() : size.width()) - 1 - q.x());
    return q;
}

// The same picture and depth map as the frame of depthFrame(), each turned by the pixels, as a
// video that was recorded the right way up would hold them (the control of a rotated frame)
QVideoFrame preTurnedDepthFrame(const QVideoFrame &source, int w, int h, int degrees, bool mirrored)
{
    const QImage picture = turned(lumaOf(source, QRect(0, 0, w, h)), degrees, mirrored);
    const QImage depth = turned(lumaOf(source, QRect(w, 0, w, h)), degrees, mirrored);
    QVideoFrameFormat format(QSize(2 * picture.width(), picture.height()), QVideoFrameFormat::Format_YUV444P);
    format.setColorRange(QVideoFrameFormat::ColorRange_Full);
    format.setStereoMode(Mode::DepthSideBySide);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly))
        return {};
    for (int y = 0; y < picture.height(); ++y) {
        for (int x = 0; x < picture.width(); ++x) {
            frame.bits(0)[y * frame.bytesPerLine(0) + x] = picture.constScanLine(y)[x];
            frame.bits(0)[y * frame.bytesPerLine(0) + picture.width() + x] = depth.constScanLine(y)[x];
        }
        for (int x = 0; x < 2 * picture.width(); ++x) {
            frame.bits(1)[y * frame.bytesPerLine(1) + x] = 128;
            frame.bits(2)[y * frame.bytesPerLine(2) + x] = 128;
        }
    }
    frame.unmap();
    return frame;
}

} // namespace

namespace {

// A picture and its depth map side by side, from two gray images, in a pixel format with chroma
// subsampling (YUV 4:2:0) or interleaved samples (RGBA, the depth in green)
QVideoFrame grayDepthFrame(const QImage &picture, const QImage &depth, QVideoFrameFormat::PixelFormat pixelFormat)
{
    const int w = picture.width(), h = picture.height();
    QVideoFrameFormat format(QSize(2 * w, h), pixelFormat);
    format.setColorRange(QVideoFrameFormat::ColorRange_Full);
    format.setStereoMode(Mode::DepthSideBySide);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly))
        return {};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < 2 * w; ++x) {
            const uchar v = x < w ? picture.constScanLine(y)[x] : depth.constScanLine(y)[x - w];
            if (pixelFormat == QVideoFrameFormat::Format_RGBA8888) {
                uchar *px = frame.bits(0) + y * frame.bytesPerLine(0) + 4 * x;
                px[0] = px[1] = px[2] = v;
                px[3] = 255;
            } else {
                frame.bits(0)[y * frame.bytesPerLine(0) + x] = v;
            }
        }
    if (pixelFormat == QVideoFrameFormat::Format_YUV420P)
        for (int plane = 1; plane <= 2; ++plane)
            for (int y = 0; y < h / 2; ++y)
                memset(frame.bits(plane) + y * frame.bytesPerLine(plane), 128, w);
    frame.unmap();
    return frame;
}

// The picture of a frame of grayDepthFrame(), the pixels of one component
QImage componentOf(const QVideoFrame &frame, QRect region, int component)
{
    QVideoFrame f = frame;
    QImage image(region.size(), QImage::Format_Grayscale8);
    if (!f.map(QVideoFrame::ReadOnly))
        return {};
    const bool bgra = f.pixelFormat() == QVideoFrameFormat::Format_RGBA8888;
    for (int y = 0; y < region.height(); ++y)
        for (int x = 0; x < region.width(); ++x)
            image.scanLine(y)[x] = bgra ? f.bits(0)[(region.y() + y) * f.bytesPerLine(0) + 4 * (region.x() + x) + component]
                                        : f.bits(0)[(region.y() + y) * f.bytesPerLine(0) + region.x() + x];
    f.unmap();
    return image;
}

} // namespace

void tst_QQuickVideoStereo::depthRotatedFormats_data()
{
    QTest::addColumn<int>("pixelFormat");
    QTest::addColumn<int>("degrees");
    QTest::newRow("YUV 4:2:0, 90") << int(QVideoFrameFormat::Format_YUV420P) << 90;
    QTest::newRow("YUV 4:2:0, 270") << int(QVideoFrameFormat::Format_YUV420P) << 270;
    QTest::newRow("YUV 4:2:0, 180") << int(QVideoFrameFormat::Format_YUV420P) << 180;
}

// The formats whose samples are subsampled or interleaved are turned plane by plane: the drawn frame
// keeps the pixel format, the picture is the turned one and the box is at its parallax
void tst_QQuickVideoStereo::depthRotatedFormats()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QFETCH(int, pixelFormat);
    QFETCH(int, degrees);
    const int w = 240, h = 136;
    const QVideoFrame luma = depthFrame(Mode::DepthSideBySide, false, w, h);
    const QImage picture = lumaOf(luma, QRect(0, 0, w, h));
    const QImage depth = lumaOf(luma, QRect(w, 0, w, h));
    QVideoFrame frame = grayDepthFrame(picture, depth, QVideoFrameFormat::PixelFormat(pixelFormat));
    QVERIFY(frame.isValid());
    frame.setRotation(QtVideo::Rotation(degrees));
    const QSize shown = degrees % 180 ? QSize(h, w) : QSize(w, h);
    QQuickVideoDepth::Renderer renderer;
    QVideoFrame pair;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, {}, &pair),
             QQuickVideoDepth::Renderer::Result::Drawn);
    QCOMPARE(pair.size(), QSize(2 * shown.width(), shown.height()));
    QCOMPARE(int(pair.pixelFormat()), pixelFormat);
    QCOMPARE(componentOf(pair, QRect(QPoint(0, 0), shown), 1), turned(picture, degrees, false));
    const QPoint box = turnedPoint(QPoint(w * 3 / 8, h / 2), QSize(w, h), degrees, false);
    const int expected = qRound(expectedParallax({}, 160, shown.width()));
    if (pixelFormat == QVideoFrameFormat::Format_YUV420P) {
        QCOMPARE(shiftAt(pair, box.y(), box.x(), 12), expected);
    } else {
        // interleaved samples: the box moved by its parallax, sample for sample, in the green channel
        const QImage left = componentOf(pair, QRect(QPoint(0, 0), shown), 1);
        const QImage right = componentOf(pair, QRect(QPoint(shown.width(), 0), shown), 1);
        int matching = 0;
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx)
                matching += left.constScanLine(box.y() + dy)[box.x() + dx]
                        == right.constScanLine(box.y() + dy)[box.x() + dx + expected];
        QCOMPARE(matching, 81);
    }
}

void tst_QQuickVideoStereo::depthRotated_data()
{
    QTest::addColumn<int>("degrees");
    QTest::addColumn<bool>("mirrored");
    QTest::newRow("90") << 90 << false;
    QTest::newRow("180") << 180 << false;
    QTest::newRow("270") << 270 << false;
    QTest::newRow("90, mirrored") << 90 << true;
    QTest::newRow("180, mirrored") << 180 << true;
}

// A depth video recorded sideways or upside down: its two views are drawn the right way up, with
// the parallax along the width of the picture as it is shown, and no rotation is left on them
void tst_QQuickVideoStereo::depthRotated()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QFETCH(int, degrees);
    QFETCH(bool, mirrored);
    const int w = 240, h = 136;
    QVideoFrame frame = depthFrame(Mode::DepthSideBySide, false, w, h);
    QVERIFY(frame.isValid());
    const QImage picture = lumaOf(frame, QRect(0, 0, w, h));
    frame.setRotation(QtVideo::Rotation(degrees));
    frame.setMirrored(mirrored);
    const QSize shown = degrees % 180 ? QSize(h, w) : QSize(w, h);

    QQuickVideoDepth::Renderer renderer;
    QVideoFrame pair;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, {}, &pair),
             QQuickVideoDepth::Renderer::Result::Drawn);
    QCOMPARE(pair.size(), QSize(2 * shown.width(), shown.height()));
    QCOMPARE(pair.rotation(), QtVideo::Rotation::None);
    QVERIFY(!pair.mirrored());
    QCOMPARE(pair.surfaceFormat().rotation(), QtVideo::Rotation::None);
    QVERIFY(!pair.surfaceFormat().isMirrored());
    // the left eye is the picture, turned: to the sample
    QCOMPARE(lumaOf(pair, QRect(QPoint(0, 0), shown)), turned(picture, degrees, mirrored));
    // the background (64) and the box (160) at their parallax along the shown width
    const QPoint background = turnedPoint(QPoint(w * 3 / 4, h / 8), QSize(w, h), degrees, mirrored);
    const QPoint box = turnedPoint(QPoint(w * 3 / 8, h / 2), QSize(w, h), degrees, mirrored);
    QCOMPARE(shiftAt(pair, background.y(), background.x(), 12),
             qRound(expectedParallax({}, 64, shown.width())));
    QCOMPARE(shiftAt(pair, box.y(), box.x(), 12), qRound(expectedParallax({}, 160, shown.width())));
}

// The size a rotated depth video is shown at is the turned picture's
void tst_QQuickVideoStereo::depthRotatedInVideoOutput()
{
    QQuickVideoOutput output;
    const int w = 240, h = 136;
    QVideoFrame frame = depthFrame(Mode::DepthSideBySide, false, w, h);
    frame.setRotation(QtVideo::Rotation::Clockwise90);
    output.videoSink()->setVideoFrame(frame);
    QTRY_COMPARE(output.stereoPacking(), QQuickVideoOutput::StereoDepthSideBySide);
    QTRY_COMPARE(output.implicitWidth(), qreal(h));
    QCOMPARE(output.implicitHeight(), qreal(w));
    // the picture alone, and the frame as stored, keep turning with the frame
    output.setStereoDepth(false);
    QTRY_COMPARE(output.implicitWidth(), qreal(h));
    QCOMPARE(output.implicitHeight(), qreal(w));
    output.setStereoView(QQuickVideoOutput::StereoFlat);
    QTRY_COMPARE(output.implicitWidth(), qreal(h));
    QCOMPARE(output.implicitHeight(), qreal(2 * w));
}

void tst_QQuickVideoStereo::depthRotatedDrawn_data()
{
    QTest::addColumn<int>("degrees");
    QTest::addColumn<bool>("mirrored");
    QTest::newRow("90") << 90 << false;
    QTest::newRow("180") << 180 << false;
    QTest::newRow("270") << 270 << false;
    QTest::newRow("90, mirrored") << 90 << true;
}

// What the scene graph shows of each eye of a rotated depth video is what it shows of the same
// video recorded the right way up (the control: the frame turned by its pixels)
void tst_QQuickVideoStereo::depthRotatedDrawn()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    if (QGuiApplication::platformName() == u"minimal" || QGuiApplication::platformName() == u"offscreen")
        QSKIP("Requires a platform with a window to render");
    QFETCH(int, degrees);
    QFETCH(bool, mirrored);
    const int w = 240, h = 136;
    const QSize shown = degrees % 180 ? QSize(h, w) : QSize(w, h);
    QQuickWindow window;
    window.resize(shown);
    window.setColor(Qt::black);
    QQuickVideoOutput output;
    output.setParentItem(window.contentItem());
    output.setSize(QSizeF(shown));
    output.setFillMode(QQuickVideoOutput::Stretch);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    const QVideoFrame upright = depthFrame(Mode::DepthSideBySide, false, w, h);
    QVideoFrame rotated = upright;
    rotated.setRotation(QtVideo::Rotation(degrees));
    rotated.setMirrored(mirrored);
    const QVideoFrame control = preTurnedDepthFrame(upright, w, h, degrees, mirrored);
    QVERIFY(control.isValid());

    for (auto eye : { QQuickVideoOutput::StereoLeftEye, QQuickVideoOutput::StereoRightEye }) {
        output.setStereoView(eye);
        output.videoSink()->setVideoFrame(control);
        QTest::qWait(300);
        const QImage expected = window.grabWindow().convertToFormat(QImage::Format_RGB32);
        output.videoSink()->setVideoFrame(rotated);
        QTest::qWait(300);
        const QImage actual = window.grabWindow().convertToFormat(QImage::Format_RGB32);
        QCOMPARE(actual.size(), expected.size());
        QVERIFY(!actual.isNull());
        // a blank frame is no witness: the picture is noise
        QVERIFY(actual.pixelColor(shown.width() / 2, shown.height() / 2)
                != actual.pixelColor(shown.width() / 2 + 3, shown.height() / 2 + 5)
                || actual.pixelColor(10, 10) != actual.pixelColor(30, 40));
        int worst = 0;
        for (int y = 0; y < actual.height(); ++y)
            for (int x = 0; x < actual.width(); ++x) {
                const QRgb a = actual.pixel(x, y), b = expected.pixel(x, y);
                worst = std::max({ worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                                   std::abs(qBlue(a) - qBlue(b)) });
            }
        QVERIFY2(worst <= 2, qPrintable(u"largest difference %1 in eye %2"_s.arg(worst).arg(int(eye))));
    }
}

QTEST_MAIN(tst_QQuickVideoStereo)
#include "tst_qquickvideostereo.moc"
