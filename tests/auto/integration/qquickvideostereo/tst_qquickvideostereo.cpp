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
#include <QtMultimedia/private/qhwvideobuffer_p.h>
#include <QtMultimedia/private/qvideoframe_p.h>
#include <QtGui/qopenglcontext.h>
#include <QtGui/qopenglextrafunctions.h>
#include <rhi/qrhi.h>
#include <QtMultimediaQuick/private/qquickvideooutput_p.h>
#include <QtMultimediaQuick/private/qquickvideostereo_p.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtMultimediaQuick/private/qquickvideodepth_p.h>
#include <QtCore/qrandom.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qtransform.h>
#include <QtQuick/qquickwindow.h>
#include <QtQuick/private/qquickwindow_p.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <tuple>
#include <cmath>
#include <set>
#include <thread>

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
    void depthDefaultsAreTheLibrarys();
    void depthWayIsAutomaticUntilPicked();
    void depthChoiceIsTold();
    void depthOnTheGpu();
    void depthOnTheGpuFromSeveralThreads();
    void depthOnTheGpuConvertsTheColour_data();
    void depthOnTheGpuConvertsTheColour();
    void depthOnTheGpuKeepsTheDepth_data();
    void depthOnTheGpuKeepsTheDepth();
    void depthImportsHardwareFrames_data();
    void depthImportsHardwareFrames();
    void depthRefusesEmptyImportedPlanes();
    void depthDeepCases_data();
    void depthDeepCases();
    void depthInVideoOutput();
    void depthViewsReachTheSinkAsTextures_data();
    void depthViewsReachTheSinkAsTextures();
    void depthTextureFramesConvertLikeTheCopies_data();
    void depthTextureFramesConvertLikeTheCopies();
    void depthTextureFrameOutlivesTheRenderer();
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
QVideoFrame depthFrame(Mode mode, bool depthFirst, int w, int h, double fps = 0)
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
    if (fps > 0)
        format.setStreamFrameRate(fps);
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

// The shift of the green of row y around x from the left view to the right one of an RGBA pair
double shiftAtRgba(const QVideoFrame &pair, int y, int x, int search)
{
    QVideoFrame f = pair;
    if (!f.map(QVideoFrame::ReadOnly))
        return qQNaN();
    const int w = f.width() / 2, half = 4;
    const uchar *p = f.bits(0);
    const int stride = f.bytesPerLine(0);
    double best = 1e30;
    int bestDx = 0;
    for (int dx = -search; dx <= search; ++dx) {
        double cost = 0;
        for (int yy = y - half; yy <= y + half; ++yy)
            for (int xx = x - half; xx <= x + half; ++xx)
                cost += std::abs(int(p[yy * stride + 4 * xx + 1])
                                 - int(p[yy * stride + 4 * (w + xx + dx) + 1]));
        if (cost < best) {
            best = cost;
            bestDx = dx;
        }
    }
    f.unmap();
    return bestDx;
}

// The way the views were drawn with when these tests were written, on the processor: forward warping,
// background filling, no automatic choice (the geometry of the views does not depend on the way)
QQuickVideoDepth::Options fixedWay()
{
    QQuickVideoDepth::Options o;
    o.automatic = false;
    o.warp = 0;
    o.fill = 0;
    o.gpu = false;
    return o;
}

double expectedParallax(const QString &mark, int sample, int width)
{
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
    QCOMPARE(renderer.draw(frame, Mode(mode), depthFirst, {}, fixedWay(), &pair),
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
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, u"farness"_s, fixedWay(), &pair),
             QQuickVideoDepth::Renderer::Result::Drawn);
    QCOMPARE(shiftAt(pair, h / 2, w * 3 / 8, 12), -qRound(expectedParallax({}, 160, w)));
    // the user's meaning comes before it, and the user's mark before both
    auto o = fixedWay();
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
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, u"nearness zero=abc"_s, fixedWay(), &pair),
             QQuickVideoDepth::Renderer::Result::PictureAlone);
}

void tst_QQuickVideoStereo::depthDefaultsAreTheLibrarys()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    // the one default of libstereo-dibr (stereo_dibr_options_default()), which Options reads and holds no copy of: layered
    // warping, depth-aided filling, 58 pixels, the screen in the middle, the way automatic, the GPU when it can
    const QQuickVideoDepth::Options o;
    QCOMPARE(o.view, 0);
    QCOMPARE(o.warp, 2);
    QCOMPARE(o.fill, 1);
    QCOMPARE(o.smooth, 0);
    QCOMPARE(o.radius, 5);
    QCOMPARE(o.range, 58.0);
    QCOMPARE(o.zero, 0.5);
    QCOMPARE(o.shift, 0.0);
    QVERIFY(o.automatic);
    QVERIFY(o.gpu);
    QQuickVideoOutput output;
    QCOMPARE(int(output.stereoDepthWarp()), o.warp);
    QCOMPARE(int(output.stereoDepthFill()), o.fill);
    QCOMPARE(output.stereoDepthRange(), o.range);
    QCOMPARE(output.stereoDepthRadius(), o.radius);
    QVERIFY(output.stereoDepthAutomatic());
    QVERIFY(output.stereoDepthGpu());
    QVERIFY(output.stereoDepthChoice().isEmpty());
}

void tst_QQuickVideoStereo::depthWayIsAutomaticUntilPicked()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QQuickVideoOutput output;
    QSignalSpy automatic(&output, &QQuickVideoOutput::stereoDepthAutomaticChanged);
    QSignalSpy fill(&output, &QQuickVideoOutput::stereoDepthFillChanged);
    QVERIFY(output.stereoDepthAutomatic());
    // picking a gap filling is picking the way, whatever it is
    output.setStereoDepthFill(QQuickVideoOutput::StereoDepthFillPyramid);
    QVERIFY(!output.stereoDepthAutomatic());
    QCOMPARE(automatic.count(), 1);
    QCOMPARE(fill.count(), 1);
    QCOMPARE(output.stereoDepthFill(), QQuickVideoOutput::StereoDepthFillPyramid);
    // the choice is given back
    output.setStereoDepthAutomatic(true);
    QVERIFY(output.stereoDepthAutomatic());
    QCOMPARE(automatic.count(), 2);
    // a warping picked, even the one it already has
    output.setStereoDepthWarp(output.stereoDepthWarp());
    QVERIFY(!output.stereoDepthAutomatic());
    QCOMPARE(automatic.count(), 3);
    // the same pick again changes nothing
    output.setStereoDepthWarp(output.stereoDepthWarp());
    QCOMPARE(automatic.count(), 3);
}

void tst_QQuickVideoStereo::depthChoiceIsTold()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    QQuickVideoOutput output;
    QSignalSpy choice(&output, &QQuickVideoOutput::stereoDepthChoiceChanged);
    const int w = 240, h = 136;
    output.videoSink()->setVideoFrame(depthFrame(Mode::DepthSideBySide, false, w, h, 25));
    QTRY_VERIFY(!output.stereoDepthChoice().isEmpty());
    QVariantMap c = output.stereoDepthChoice();
    // the way, whether it keeps up, what a frame may take at 25 frames a second (four fifths of 40 ms), and where it was drawn
    QVERIFY(c.contains(u"warp"_s) && c.contains(u"fill"_s) && c.contains(u"keepsUp"_s) && c.contains(u"onGpu"_s));
    QCOMPARE(c.value(u"fps"_s).toInt(), 25);
    QCOMPARE(c.value(u"budgetMs"_s).toDouble(), 32.0);
    QVERIFY(!c.value(u"gpu"_s).toString().isEmpty());
    QTRY_VERIFY(choice.count() >= 1);
    // what the GPU has free, for the controls to say beside a way that would not fit: the size and depth the views are drawn at (and the
    // free bytes when the driver says, -1 when it does not); told for the GPU, and for the person's own way too
    if (c.value(u"onGpu"_s).toBool()) {
        QVariantMap m = output.stereoDepthGpuMemory();
        QTRY_VERIFY(!(m = output.stereoDepthGpuMemory()).isEmpty());
        QCOMPARE(m.value(u"width"_s).toInt(), w);
        QCOMPARE(m.value(u"height"_s).toInt(), h);
        QCOMPARE(m.value(u"bits"_s).toInt(), 8);
        QVERIFY(m.contains(u"freeBytes"_s));
    }
    // the timing was kept in the person's cache, and in nothing else
    QVERIFY(QFileInfo::exists(cache.path() + u"/stereo-dibr/timings"_s));
    // the person picks the way: no choice is told
    output.setStereoDepthWarp(QQuickVideoOutput::StereoDepthForward);
    QTRY_VERIFY(output.stereoDepthChoice().isEmpty());
    // and gives it back
    output.setStereoDepthAutomatic(true);
    QTRY_VERIFY(!output.stereoDepthChoice().isEmpty());
    // the processor asked for: the C code, said
    output.setStereoDepthGpu(false);
    QTRY_COMPARE(output.stereoDepthChoice().value(u"onGpu"_s).toBool(), false);
    QTRY_VERIFY(output.stereoDepthGpuMemory().isEmpty()); // the processor draws: no GPU memory to tell
    // a video without depth tells nothing
    QVideoFrameFormat plain(QSize(64, 48), QVideoFrameFormat::Format_YUV420P);
    QVideoFrame frame(plain);
    output.videoSink()->setVideoFrame(frame);
    QTRY_VERIFY(output.stereoDepthChoice().isEmpty());
    qunsetenv("XDG_CACHE_HOME");
}

void tst_QQuickVideoStereo::depthOnTheGpu()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    const int w = 240, h = 136;
    const QVideoFrame frame = depthFrame(Mode::DepthSideBySide, false, w, h);
    auto o = fixedWay();
    o.gpu = true;
    QQuickVideoDepth::Renderer renderer;
    QVideoFrame gpu;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &gpu),
             QQuickVideoDepth::Renderer::Result::Drawn);
    if (!renderer.lastOnGpu())
        QSKIP("no OpenGL context in this environment: the C code drew the views, and said so");
    // RGBA 8 bit, full side by side, each view at the picture's size, the left eye first
    QCOMPARE(gpu.pixelFormat(), QVideoFrameFormat::Format_RGBA8888);
    QCOMPARE(gpu.size(), QSize(2 * w, h));
    QCOMPARE(gpu.surfaceFormat().stereoMode(), Mode::SideBySide);
    // the background (64) and the box (160) at their parallax, to the pixel, as on the processor
    QCOMPARE(shiftAtRgba(gpu, h / 8, w * 3 / 4, 12), qRound(expectedParallax({}, 64, w)));
    QCOMPARE(shiftAtRgba(gpu, h / 2, w * 3 / 8, 12), qRound(expectedParallax({}, 160, w)));
    // the same views as the C code draws: the luma of its frame (the chroma is neutral) is the green of this one
    o.gpu = false;
    QVideoFrame cpu;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &cpu),
             QQuickVideoDepth::Renderer::Result::Drawn);
    QVERIFY(!renderer.lastOnGpu());
    QCOMPARE(cpu.pixelFormat(), QVideoFrameFormat::Format_YUV444P);
    QVERIFY(gpu.map(QVideoFrame::ReadOnly));
    QVERIFY(cpu.map(QVideoFrame::ReadOnly));
    long far = 0, sum = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < 2 * w; ++x) {
            const int d = std::abs(int(gpu.bits(0)[y * gpu.bytesPerLine(0) + 4 * x + 1])
                                   - int(cpu.bits(0)[y * cpu.bytesPerLine(0) + x]));
            sum += d;
            far += d > 2;
        }
    gpu.unmap();
    cpu.unmap();
    QVERIFY2(far * 500 < long(2 * w) * h, qPrintable(u"pixels off by more than 2: %1"_s.arg(far)));
    QVERIFY2(sum < long(2 * w) * h / 4, qPrintable(u"sum of differences: %1"_s.arg(sum)));
    qunsetenv("XDG_CACHE_HOME");
}

void tst_QQuickVideoStereo::depthOnTheGpuFromSeveralThreads()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    // the frames of a video reach VideoOutput from the thread of the decoder, of the item and of the sink in turn: an OpenGL context
    // belongs to one thread, so the drawing runs in a thread of its own (a context made current in another thread aborts)
    const int w = 96, h = 54;
    const QVideoFrame frame = depthFrame(Mode::DepthSideBySide, false, w, h);
    auto o = fixedWay();
    o.gpu = true;
    QQuickVideoDepth::Renderer renderer;
    QVideoFrame first;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &first), QQuickVideoDepth::Renderer::Result::Drawn);
    const bool onGpu = renderer.lastOnGpu();
    for (int i = 0; i < 6; ++i) {
        QQuickVideoDepth::Renderer::Result result = QQuickVideoDepth::Renderer::Result::Failed;
        QVideoFrame drawn;
        if (i % 2) {
            result = renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &drawn);
        } else {
            std::thread other([&] { result = renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &drawn); });
            other.join();
        }
        QCOMPARE(result, QQuickVideoDepth::Renderer::Result::Drawn);
        QCOMPARE(renderer.lastOnGpu(), onGpu);
        QCOMPARE(drawn.size(), first.size());
    }
    qunsetenv("XDG_CACHE_HOME");
}

void tst_QQuickVideoStereo::depthOnTheGpuConvertsTheColour_data()
{
    QTest::addColumn<int>("format");
    QTest::addColumn<int>("space");
    QTest::addColumn<bool>("full");
    QTest::newRow("YUV 4:2:0, BT.601, limited") << int(QVideoFrameFormat::Format_YUV420P) << int(QVideoFrameFormat::ColorSpace_BT601) << false;
    QTest::newRow("YUV 4:2:0, BT.709, limited") << int(QVideoFrameFormat::Format_YUV420P) << int(QVideoFrameFormat::ColorSpace_BT709) << false;
    QTest::newRow("NV12, BT.709, limited") << int(QVideoFrameFormat::Format_NV12) << int(QVideoFrameFormat::ColorSpace_BT709) << false;
    QTest::newRow("NV21, BT.601, full") << int(QVideoFrameFormat::Format_NV21) << int(QVideoFrameFormat::ColorSpace_BT601) << true;
    QTest::newRow("YV12, BT.2020, limited") << int(QVideoFrameFormat::Format_YV12) << int(QVideoFrameFormat::ColorSpace_BT2020) << false;
}

void tst_QQuickVideoStereo::depthOnTheGpuConvertsTheColour()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QFETCH(int, format);
    QFETCH(int, space);
    QFETCH(bool, full);
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    // a picture of one colour (Y 150, U 100, V 200) and a flat depth, side by side
    const int w = 64, h = 48, Y = 150, U = 100, V = 200;
    QVideoFrameFormat f(QSize(2 * w, h), QVideoFrameFormat::PixelFormat(format));
    f.setColorSpace(QVideoFrameFormat::ColorSpace(space));
    f.setColorRange(full ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
    f.setStereoMode(Mode::DepthSideBySide);
    QVideoFrame frame(f);
    QVERIFY(frame.map(QVideoFrame::WriteOnly));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < 2 * w; ++x)
            frame.bits(0)[y * frame.bytesPerLine(0) + x] = x < w ? Y : 128;
    const bool semi = format == QVideoFrameFormat::Format_NV12 || format == QVideoFrameFormat::Format_NV21;
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w; ++x) {
            // chroma of the picture (the left half) U, V; of the depth map neutral
            const int u = x < w / 2 ? U : 128, v = x < w / 2 ? V : 128;
            if (semi) {
                const bool nv21 = format == QVideoFrameFormat::Format_NV21;
                frame.bits(1)[y * frame.bytesPerLine(1) + 2 * x] = nv21 ? v : u;
                frame.bits(1)[y * frame.bytesPerLine(1) + 2 * x + 1] = nv21 ? u : v;
            } else {
                const bool yv12 = format == QVideoFrameFormat::Format_YV12;
                frame.bits(yv12 ? 2 : 1)[y * frame.bytesPerLine(yv12 ? 2 : 1) + x] = u;
                frame.bits(yv12 ? 1 : 2)[y * frame.bytesPerLine(yv12 ? 1 : 2) + x] = v;
            }
        }
    frame.unmap();
    QQuickVideoDepth::Renderer renderer;
    QVideoFrame pair;
    auto o = fixedWay();
    o.gpu = true;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &pair), QQuickVideoDepth::Renderer::Result::Drawn);
    if (!renderer.lastOnGpu())
        QSKIP("no OpenGL context in this environment");
    // the left view is the picture as it is: its RGB, from the matrix of the colour space
    double kr = 0.299, kb = 0.114;
    if (space == QVideoFrameFormat::ColorSpace_BT709) {
        kr = 0.2126;
        kb = 0.0722;
    } else if (space == QVideoFrameFormat::ColorSpace_BT2020) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double yy = full ? Y / 255.0 : (Y - 16) / 219.0;
    const double cb = full ? (U - 128) / 255.0 : (U - 128) / 224.0, cr = full ? (V - 128) / 255.0 : (V - 128) / 224.0;
    const double r = yy + 2 * (1 - kr) * cr, b = yy + 2 * (1 - kb) * cb, g = (yy - kr * r - kb * b) / (1 - kr - kb);
    QVERIFY(pair.map(QVideoFrame::ReadOnly));
    const uchar *px = pair.bits(0) + (h / 2) * pair.bytesPerLine(0) + 4 * (w / 4);
    auto clamped = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    QVERIFY2(std::abs(px[0] - clamped(r)) <= 2 && std::abs(px[1] - clamped(g)) <= 2 && std::abs(px[2] - clamped(b)) <= 2 && px[3] == 255,
             qPrintable(u"RGBA %1 %2 %3 %4, expected %5 %6 %7"_s.arg(px[0]).arg(px[1]).arg(px[2]).arg(px[3]).arg(clamped(r)).arg(clamped(g)).arg(clamped(b))));
    pair.unmap();
    qunsetenv("XDG_CACHE_HOME");
}

void tst_QQuickVideoStereo::depthOnTheGpuKeepsTheDepth_data()
{
    QTest::addColumn<int>("format");
    QTest::addColumn<int>("bits");   // the depth of the content
    QTest::addColumn<int>("shift");  // the code's place in the 16 bit word (P010 keeps it in the high bits)
    QTest::addColumn<int>("space");
    QTest::addColumn<bool>("full");
    const auto yuv = QVideoFrameFormat::Format_YUV420P10;
    QTest::newRow("YUV 4:2:0 10 bit, BT.709, limited") << int(yuv) << 10 << 0 << int(QVideoFrameFormat::ColorSpace_BT709) << false;
    QTest::newRow("YUV 4:2:0 10 bit, BT.601, full") << int(yuv) << 10 << 0 << int(QVideoFrameFormat::ColorSpace_BT601) << true;
    QTest::newRow("P010, BT.2020, limited") << int(QVideoFrameFormat::Format_P010) << 10 << 6 << int(QVideoFrameFormat::ColorSpace_BT2020) << false;
    QTest::newRow("P016, BT.709, limited") << int(QVideoFrameFormat::Format_P016) << 16 << 0 << int(QVideoFrameFormat::ColorSpace_BT709) << false;
    // 12 bit: yuv420p12, yuv422p12, yuv444p12 and NVDEC's P012 reach the passes as P016, YUV422P16 and YUV444P16 with the code in the high 12 bits
    QTest::newRow("12 bit in P016 (P012, yuv420p12), BT.709, limited") << int(QVideoFrameFormat::Format_P016) << 12 << 4 << int(QVideoFrameFormat::ColorSpace_BT709) << false;
    QTest::newRow("12 bit in YUV422P16 (yuv422p12), BT.601, full") << int(QVideoFrameFormat::Format_YUV422P16) << 12 << 4 << int(QVideoFrameFormat::ColorSpace_BT601) << true;
    QTest::newRow("12 bit in YUV444P16 (yuv444p12), BT.2020, limited") << int(QVideoFrameFormat::Format_YUV444P16) << 12 << 4 << int(QVideoFrameFormat::ColorSpace_BT2020) << false;
    QTest::newRow("YUV 4:2:2 16 bit, BT.601, full") << int(QVideoFrameFormat::Format_YUV422P16) << 16 << 0 << int(QVideoFrameFormat::ColorSpace_BT601) << true;
    QTest::newRow("YUV 4:4:4 16 bit, BT.709, limited") << int(QVideoFrameFormat::Format_YUV444P16) << 16 << 0 << int(QVideoFrameFormat::ColorSpace_BT709) << false;
}

// The frames of the GPU keep the depth of the source: a ramp that climbs one code of the source (10 bits) or a few words (16 bits)
// a pixel is a run of equal samples after an 8 bit truncation, and a flat colour whose low bits are odd must come back to the
// word within the error of the conversion; both as the C code draws them
void tst_QQuickVideoStereo::depthOnTheGpuKeepsTheDepth()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QFETCH(int, format);
    QFETCH(int, bits);
    QFETCH(int, shift);
    QFETCH(int, space);
    QFETCH(bool, full);
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    const auto pf = QVideoFrameFormat::PixelFormat(format);
    const bool semi = pf == QVideoFrameFormat::Format_P010 || pf == QVideoFrameFormat::Format_P016;
    const int log2w = pf == QVideoFrameFormat::Format_YUV444P16 ? 0 : 1;
    const int log2h = (pf == QVideoFrameFormat::Format_YUV422P16 || pf == QVideoFrameFormat::Format_YUV444P16) ? 0 : 1;
    const int w = 128, h = 64;
    const int unit = 1 << shift;
    // 12 bit content is held in the high bits of 16 bit words (P016 and the 16 bit planar formats: the unit is 16 words, as Qt's FFmpeg backend
    // converts yuv420p12 and the other 12 bit formats and as NVDEC's P012 is read), a code a step as the 10 bit ramp has
    const int lo = bits == 10 ? 300 : bits == 12 ? 1200 : 20000, step = bits == 16 ? 7 : 1;
    const int maxCode = (1 << bits) - 1;
    const int Ucode = bits == 10 ? 517 : bits == 12 ? 2059 : 33001, Vcode = bits == 10 ? 507 : bits == 12 ? 2003 : 32001;
    QVideoFrameFormat f(QSize(2 * w, h), pf);
    f.setColorSpace(QVideoFrameFormat::ColorSpace(space));
    f.setColorRange(full ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
    f.setStereoMode(Mode::DepthSideBySide);
    QVideoFrame frame(f);
    QVERIFY(frame.map(QVideoFrame::WriteOnly));
    auto word = [&](int plane, int x, int y) { return reinterpret_cast<quint16 *>(frame.bits(plane) + y * frame.bytesPerLine(plane)) + x; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < 2 * w; ++x)
            *word(0, x, y) = quint16((x < w ? lo + x * step : (160 * maxCode) / 255) * unit); // the ramp, and the depth: flat
    const int cw = (2 * w) >> log2w, ch = h >> log2h;
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const bool picture = (x << log2w) < w;
            const quint16 u = quint16((picture ? Ucode : (maxCode + 1) / 2) * unit), v = quint16((picture ? Vcode : (maxCode + 1) / 2) * unit);
            if (semi) {
                *word(1, 2 * x, y) = u;
                *word(1, 2 * x + 1, y) = v;
            } else {
                *word(1, x, y) = u;
                *word(2, x, y) = v;
            }
        }
    frame.unmap();

    QQuickVideoDepth::Renderer renderer;
    auto o = fixedWay();
    QVideoFrame cpu;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &cpu), QQuickVideoDepth::Renderer::Result::Drawn);
    QVERIFY(!renderer.lastOnGpu());
    o.gpu = true;
    QVideoFrame gpu;
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &gpu), QQuickVideoDepth::Renderer::Result::Drawn);
    if (!renderer.lastOnGpu())
        QSKIP("no OpenGL context in this environment");
    // the frame of the source's format and depth, full side by side
    QCOMPARE(gpu.pixelFormat(), pf);
    QCOMPARE(gpu.size(), QSize(2 * w, h));
    QCOMPARE(gpu.surfaceFormat().stereoMode(), Mode::SideBySide);
    QVERIFY(gpu.map(QVideoFrame::ReadOnly));
    QVERIFY(cpu.map(QVideoFrame::ReadOnly));
    auto code = [&](const QVideoFrame &fr, int plane, int x, int y) {
        return int(*(reinterpret_cast<const quint16 *>(fr.bits(plane) + y * fr.bytesPerLine(plane)) + x)) / unit;
    };
    // the ramp, in both views: the codes within the error of the conversion of those the C code draws, none of them lost
    const int tolerance = bits == 10 ? 1 : bits == 12 ? 2 : 24;
    auto run = [&](const QVideoFrame &fr, int x0, int x1, int y) {
        std::set<int> distinct;
        for (int x = x0; x < x1; ++x)
            distinct.insert(code(fr, 0, x, y));
        return distinct;
    };
    // an 8 bit truncation of the frame: what the control must see through
    auto truncated = [&](const QVideoFrame &fr, int x0, int x1, int y) {
        std::set<int> distinct;
        for (int x = x0; x < x1; ++x)
            distinct.insert(code(fr, 0, x, y) >> (bits - 8));
        return distinct;
    };
    const int y = h / 2;
    for (const int x0 : { 12, w + 12 }) {
        const int x1 = x0 + w - 24;
        long worst = 0;
        for (int x = x0; x < x1; ++x)
            worst = std::max<long>(worst, std::abs(code(gpu, 0, x, y) - code(cpu, 0, x, y)));
        QVERIFY2(worst <= tolerance, qPrintable(u"the luma is off the C code's by %1 codes (tolerance %2)"_s.arg(worst).arg(tolerance)));
        // the ramp is a ramp at the source's depth, not at 8 bits: as many distinct codes as the C code draws, and more than an
        // 8 bit truncation of the very same frame would leave
        const size_t drawn = run(gpu, x0, x1, y).size(), reference = run(cpu, x0, x1, y).size();
        QVERIFY2(drawn * 10 >= reference * 9, qPrintable(u"%1 distinct codes, the C code draws %2"_s.arg(drawn).arg(reference)));
        const size_t cut = truncated(gpu, x0, x1, y).size();
        QVERIFY2(drawn > 2 * cut, qPrintable(u"%1 distinct codes against %2 at 8 bits: the low bits were lost"_s.arg(drawn).arg(cut)));
    }
    // the control: the same check fails on a frame that was cut to 8 bits and widened again
    {
        std::set<int> cutAndWidened;
        for (int x = 12; x < w - 12; ++x)
            cutAndWidened.insert((code(gpu, 0, x, y) >> (bits - 8)) << (bits - 8));
        const size_t drawn = run(gpu, 12, w - 12, y).size();
        QVERIFY2(cutAndWidened.size() * 2 < drawn, "the control did not see a truncation");
    }
    // the flat colour: its chroma back to the words, with the odd low bits
    const int cx = w / 2, cy = h / 2;
    for (int plane = 1; plane <= (semi ? 1 : 2); ++plane) {
        for (int c = 0; c < (semi ? 2 : 1); ++c) {
            const int expected = (semi ? c == 0 : plane == 1) ? Ucode : Vcode;
            const int at = (cx >> log2w) * (semi ? 2 : 1) + c;
            const int g = code(gpu, plane, at, cy >> log2h), r = code(cpu, plane, at, cy >> log2h);
            const int cTolerance = bits == 10 ? 2 : bits == 12 ? 4 : 24;
            QVERIFY2(std::abs(g - expected) <= cTolerance, qPrintable(u"chroma %1 of plane %2: %3, the source's %4"_s.arg(c).arg(plane).arg(g).arg(expected)));
            QVERIFY2(std::abs(g - r) <= cTolerance, qPrintable(u"chroma %1 of plane %2: %3, the C code's %4"_s.arg(c).arg(plane).arg(g).arg(r)));
        }
    }
    gpu.unmap();
    cpu.unmap();
    qunsetenv("XDG_CACHE_HOME");
}

namespace {

// A frame a decoder keeps on a GPU, as far as the passes can tell: its planes are textures of the drawing context that the decoder's
// converter makes (mapTextures() through the QRhi), and a map() is the read back to system memory, counted
struct FakeCounts
{
    std::atomic<int> maps { 0 };
    std::atomic<int> mapTextures { 0 };
};

class FakeTextures : public QVideoFrameTextures
{
public:
    FakeTextures(QRhi &rhi, const std::vector<std::tuple<std::vector<uchar>, int, int, int, int>> &planes, bool deep)
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLExtraFunctions *f = ctx->extraFunctions();
        m_context = ctx;
        for (size_t i = 0; i < planes.size(); ++i) {
            const auto &[data, stride, w, h, channels] = planes[i];
            const int bytes = deep ? 2 : 1;
            unsigned id = 0;
            f->glGenTextures(1, &id);
            f->glBindTexture(GL_TEXTURE_2D, id);
            const unsigned internal = deep ? (channels == 1 ? 0x822A : 0x822C) : (channels == 1 ? GL_R8 : GL_RG8);
            f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            f->glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / (bytes * channels));
            f->glTexImage2D(GL_TEXTURE_2D, 0, int(internal), w, h, 0, channels == 1 ? GL_RED : GL_RG,
                            deep ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, data.data());
            f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            // The filters stay as they are created, as the VA-API converter leaves its textures: a minification filter that needs mipmaps
            // (the texture is incomplete, and a pass that reads it by texel reads zeros) until the importing pass sets them
            m_ids[i] = id;
            const QRhiTexture::Format format = deep ? (channels == 1 ? QRhiTexture::R16 : QRhiTexture::RG16)
                                                    : (channels == 1 ? QRhiTexture::R8 : QRhiTexture::RG8);
            m_textures[i].reset(rhi.newTexture(format, QSize(w, h)));
            m_textures[i]->createFrom({ quint64(id), 0 });
        }
    }
    ~FakeTextures() override
    {
        m_textures[0].reset();
        m_textures[1].reset();
        if (QOpenGLContext::currentContext() == m_context)
            m_context->extraFunctions()->glDeleteTextures(2, m_ids);
    }
    QRhiTexture *texture(uint plane) const override { return plane < 2 ? m_textures[plane].get() : nullptr; }

private:
    QOpenGLContext *m_context = nullptr;
    unsigned m_ids[2] = {};
    std::unique_ptr<QRhiTexture> m_textures[2];
};

class FakeHardwareBuffer : public QHwVideoBuffer
{
public:
    using Plane = std::tuple<std::vector<uchar>, int, int, int, int>; // data, stride, width, height, channels
    FakeHardwareBuffer(QVideoFrameFormat format, std::vector<Plane> planes, bool deep, FakeCounts *counts)
        : QHwVideoBuffer(QVideoFrame::NoHandle), m_format(std::move(format)), m_planes(std::move(planes)), m_deep(deep), m_counts(counts)
    {
    }
    void initTextureConverter(QRhi &) override { m_type = QVideoFrame::RhiTextureHandle; }
    QVideoFrameTexturesUPtr mapTextures(QRhi &rhi, QVideoFrameTexturesUPtr &) override
    {
        ++m_counts->mapTextures;
        return std::make_unique<FakeTextures>(rhi, m_planes, m_deep);
    }
    MapData map(QVideoFrame::MapMode) override
    {
        ++m_counts->maps;
        MapData d;
        d.planeCount = int(m_planes.size());
        for (size_t i = 0; i < m_planes.size(); ++i) {
            d.data[i] = std::get<0>(m_planes[i]).data();
            d.bytesPerLine[i] = std::get<1>(m_planes[i]);
            d.dataSize[i] = int(std::get<0>(m_planes[i]).size());
        }
        return d;
    }
    QVideoFrameFormat format() const override { return m_format; }

private:
    QVideoFrameFormat m_format;
    std::vector<Plane> m_planes;
    bool m_deep;
    FakeCounts *m_counts;
};

} // namespace

void tst_QQuickVideoStereo::depthImportsHardwareFrames_data()
{
    QTest::addColumn<int>("format");
    QTest::newRow("NV12") << int(QVideoFrameFormat::Format_NV12);
    QTest::newRow("P010") << int(QVideoFrameFormat::Format_P010);
}

// A frame the decoder keeps on a GPU goes to the passes as its textures, with no read back to system memory; the same views come out
// as from a frame in system memory; the copy to system memory, when asked for or when the converter refuses, is the only path that
// reads back, and it is counted and said
void tst_QQuickVideoStereo::depthImportsHardwareFrames()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QFETCH(int, format);
    const auto pf = QVideoFrameFormat::PixelFormat(format);
    const bool deep = pf == QVideoFrameFormat::Format_P010;
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    const int w = 128, h = 64, bytes = deep ? 2 : 1;
    // the picture (left) is a ramp with a diagonal, the depth map (right) a box at 160 on 64
    std::vector<uchar> luma(size_t(2 * w) * h * bytes), chroma(size_t(2 * w) * (h / 2) * bytes);
    auto put = [&](std::vector<uchar> &v, size_t index, int code) {
        if (deep)
            reinterpret_cast<quint16 *>(v.data())[index] = quint16(code << 6);
        else
            v[index] = uchar(code >> 2);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < 2 * w; ++x) {
            const bool box = x - w >= w / 4 && x - w < w / 2 && y >= h / 4 && y < 3 * h / 4;
            put(luma, size_t(y) * 2 * w + x, x < w ? 200 + ((x * 3 + y * 5) % 560) : (box ? 160 * 4 : 64 * 4));
        }
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w; ++x) {
            put(chroma, size_t(y) * 2 * w + 2 * x, x < w / 2 ? 400 + x : 512);
            put(chroma, size_t(y) * 2 * w + 2 * x + 1, x < w / 2 ? 600 - x : 512);
        }
    QVideoFrameFormat f(QSize(2 * w, h), pf);
    f.setColorSpace(QVideoFrameFormat::ColorSpace_BT709);
    f.setColorRange(QVideoFrameFormat::ColorRange_Video);
    f.setStereoMode(Mode::DepthSideBySide);
    // the same frame in system memory
    QVideoFrame memory(f);
    QVERIFY(memory.map(QVideoFrame::WriteOnly));
    for (int y = 0; y < h; ++y)
        std::memcpy(memory.bits(0) + y * memory.bytesPerLine(0), luma.data() + size_t(y) * 2 * w * bytes, size_t(2 * w) * bytes);
    for (int y = 0; y < h / 2; ++y)
        std::memcpy(memory.bits(1) + y * memory.bytesPerLine(1), chroma.data() + size_t(y) * 2 * w * bytes, size_t(2 * w) * bytes);
    memory.unmap();

    auto o = fixedWay();
    o.gpu = true;
    QQuickVideoDepth::Renderer reference;
    QVideoFrame expected;
    QCOMPARE(reference.draw(memory, Mode::DepthSideBySide, false, {}, o, &expected), QQuickVideoDepth::Renderer::Result::Drawn);
    if (!reference.lastOnGpu())
        QSKIP("no OpenGL context in this environment");
    QCOMPARE(reference.lastFrameRoute(), QQuickVideoDepth::Renderer::FrameRoute::Memory);

    auto hardware = [&](FakeCounts *counts) {
        std::vector<FakeHardwareBuffer::Plane> planes;
        planes.emplace_back(luma, 2 * w * bytes, 2 * w, h, 1);
        planes.emplace_back(chroma, 2 * w * bytes, w, h / 2, 2);
        return QVideoFramePrivate::createFrame(std::make_unique<FakeHardwareBuffer>(f, std::move(planes), deep, counts), f);
    };
    auto same = [&](const QVideoFrame &a, const QVideoFrame &b) {
        QVideoFrame x = a, y = b;
        if (!x.map(QVideoFrame::ReadOnly) || !y.map(QVideoFrame::ReadOnly))
            return false;
        bool equal = x.size() == y.size() && x.pixelFormat() == y.pixelFormat();
        for (int p = 0; equal && p < x.planeCount(); ++p)
            for (int row = 0; equal && row < (p ? x.height() / 2 : x.height()); ++row)
                equal = std::memcmp(x.bits(p) + row * x.bytesPerLine(p), y.bits(p) + row * y.bytesPerLine(p), size_t(x.width()) * bytes) == 0;
        x.unmap();
        y.unmap();
        return equal;
    };

    // imported where it is: the textures are mapped, the frame is never read back, the views are the memory frame's
    {
        FakeCounts counts;
        QQuickVideoDepth::Renderer renderer;
        QVideoFrame drawn;
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(renderer.draw(hardware(&counts), Mode::DepthSideBySide, false, {}, o, &drawn),
                     QQuickVideoDepth::Renderer::Result::Drawn);
            QVERIFY(renderer.lastOnGpu());
            QCOMPARE(renderer.lastFrameRoute(), QQuickVideoDepth::Renderer::FrameRoute::Imported);
        }
        QCOMPARE(int(counts.maps), 0);
        QCOMPARE(int(counts.mapTextures), 3);
        QCOMPARE(renderer.hardwareReadBacks(), 0);
        QVERIFY(same(drawn, expected));
    }
    // the copy asked for (the negative control): the read back shows, the views are the same
    {
        FakeCounts counts;
        QQuickVideoDepth::Renderer renderer;
        QVideoFrame drawn;
        auto copy = o;
        copy.importFrames = false;
        QCOMPARE(renderer.draw(hardware(&counts), Mode::DepthSideBySide, false, {}, copy, &drawn), QQuickVideoDepth::Renderer::Result::Drawn);
        QCOMPARE(renderer.lastFrameRoute(), QQuickVideoDepth::Renderer::FrameRoute::Copied);
        QVERIFY(!renderer.lastFrameRouteWhy().isEmpty());
        QCOMPARE(int(counts.maps), 1);
        QCOMPARE(int(counts.mapTextures), 0);
        QCOMPARE(renderer.hardwareReadBacks(), 1);
        QVERIFY(same(drawn, expected));
    }
    // a converter that refuses the frame (a DMA-BUF the drawing GPU cannot import): copied, said, the same views
    {
        FakeCounts counts;
        class Refusing : public FakeHardwareBuffer
        {
        public:
            using FakeHardwareBuffer::FakeHardwareBuffer;
            QVideoFrameTexturesUPtr mapTextures(QRhi &, QVideoFrameTexturesUPtr &) override { return nullptr; }
        };
        std::vector<FakeHardwareBuffer::Plane> planes;
        planes.emplace_back(luma, 2 * w * bytes, 2 * w, h, 1);
        planes.emplace_back(chroma, 2 * w * bytes, w, h / 2, 2);
        const QVideoFrame frame = QVideoFramePrivate::createFrame(std::make_unique<Refusing>(f, std::move(planes), deep, &counts), f);
        QQuickVideoDepth::Renderer renderer;
        QVideoFrame drawn;
        QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &drawn), QQuickVideoDepth::Renderer::Result::Drawn);
        QCOMPARE(renderer.lastFrameRoute(), QQuickVideoDepth::Renderer::FrameRoute::Copied);
        QVERIFY2(renderer.lastFrameRouteWhy().contains(u"refused"_s), qPrintable(renderer.lastFrameRouteWhy()));
        QCOMPARE(int(counts.maps), 1);
        QCOMPARE(renderer.hardwareReadBacks(), 1);
        QVERIFY(same(drawn, expected));
    }
    qunsetenv("XDG_CACHE_HOME");
}

// The cases of the depth captures at 10 and 16 bits (tools/qt/make-deep-cases.py makes the frames from the cases' pictures, DEEP_CASES is their
// directory): the views the GPU draws against the C code's at the source's depth. The same criterion as the 8 bit cases, in 8 bit
// equivalents of the codes: fewer than one pixel in 500 off by more than 2, a mean under 0.25
// A DMA-BUF another GPU accepts and reads as zeros (the first import on the A10, before the filters were set): luma and chroma all zero is
// no picture, so the frames are copied from then on and the reason is said; a frame with a picture is imported (the control, above)
// The drawer (its context, its QRhi and the converters of the decoder made on it) goes with the Renderer, before the decoder's own state: a
// converter that is let go later pushes a CUDA context that is gone (a crash at the end of the first NVDEC run that handed textures to the sink).
// A frame of textures that outlives the Renderer has nothing to give and nothing to read back, and is let go without harm.
void tst_QQuickVideoStereo::depthTextureFrameOutlivesTheRenderer()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    if (qgetenv("QSG_RHI_BACKEND") != "opengl")
        QSKIP("QSG_RHI_BACKEND=opengl is not set: the sink must draw with OpenGL");
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    QQuickWindow window;
    window.resize(64, 64);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QRhi *rhi = nullptr;
    QTRY_VERIFY((rhi = QQuickWindowPrivate::get(&window)->rhi) != nullptr);
    const QVideoFrame frame = depthFrame(Mode::DepthSideBySide, false, 128, 64);
    QVideoFrame drawn;
    {
        QQuickVideoDepth::Renderer renderer;
        renderer.setSinkRhi(rhi);
        QQuickVideoDepth::Options o = fixedWay();
        o.gpu = true;
        for (int i = 0; i < 3; ++i)
            QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &drawn), QQuickVideoDepth::Renderer::Result::Drawn);
        QCOMPARE(renderer.lastViewsRoute(), QQuickVideoDepth::Renderer::ViewsRoute::Texture);
        QVERIFY(drawn.isValid());
    }
    // the Renderer is gone: the frame cannot be read back, and is let go without a crash
    QVERIFY(!drawn.map(QVideoFrame::ReadOnly));
    drawn = {};
}

void tst_QQuickVideoStereo::depthRefusesEmptyImportedPlanes()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    const int w = 128, h = 64;
    std::vector<uchar> luma(size_t(2 * w) * h, 0), chroma(size_t(2 * w) * (h / 2), 0);
    QVideoFrameFormat f(QSize(2 * w, h), QVideoFrameFormat::Format_NV12);
    f.setStereoMode(Mode::DepthSideBySide);
    FakeCounts counts;
    auto hardware = [&] {
        std::vector<FakeHardwareBuffer::Plane> planes;
        planes.emplace_back(luma, 2 * w, 2 * w, h, 1);
        planes.emplace_back(chroma, 2 * w, w, h / 2, 2);
        return QVideoFramePrivate::createFrame(std::make_unique<FakeHardwareBuffer>(f, std::move(planes), false, &counts), f);
    };
    auto o = fixedWay();
    o.gpu = true;
    QQuickVideoDepth::Renderer renderer;
    QVideoFrame drawn;
    for (int i = 0; i < 6; ++i) {
        const auto result = renderer.draw(hardware(), Mode::DepthSideBySide, false, {}, o, &drawn);
        if (result != QQuickVideoDepth::Renderer::Result::Drawn || !renderer.lastOnGpu())
            QSKIP("no OpenGL context in this environment");
    }
    QCOMPARE(renderer.lastFrameRoute(), QQuickVideoDepth::Renderer::FrameRoute::Copied);
    QVERIFY2(renderer.lastFrameRouteWhy().contains(u"empty"_s), qPrintable(renderer.lastFrameRouteWhy()));
    QVERIFY(renderer.hardwareReadBacks() >= 1);
    int empty = 0;
    QVERIFY(renderer.importChecks(&empty) >= 4);
    QVERIFY(empty >= 4);
}

void tst_QQuickVideoStereo::depthDeepCases_data()
{
    QTest::addColumn<QString>("raw");
    QTest::addColumn<QString>("format");
    QTest::addColumn<int>("w");
    QTest::addColumn<int>("h");
    QTest::addColumn<QString>("layout");
    QTest::addColumn<bool>("first");
    QTest::addColumn<QString>("mark");
    QTest::addColumn<QString>("opts");
    const QString dir = qEnvironmentVariable("DEEP_CASES");
    if (dir.isEmpty())
        return;
    QFile table(dir + u"/cases.tsv"_s);
    if (!table.open(QIODevice::ReadOnly))
        return;
    while (!table.atEnd()) {
        const QStringList c = QString::fromUtf8(table.readLine()).remove(u'\n').split(u'\t');
        if (c.size() < 9)
            continue;
        QTest::newRow(qPrintable(c[1])) << dir + u'/' + c[1] << c[2] << c[3].toInt() << c[4].toInt() << c[5] << (c[6] == u"1"_s)
                                         << (c[0].contains(u"malformed"_s) ? u"nearness zero=abc"_s : c[7]) << c[8];
    }
}

void tst_QQuickVideoStereo::depthDeepCases()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    if (qEnvironmentVariable("DEEP_CASES").isEmpty())
        QSKIP("DEEP_CASES (the frames of tools/qt/make-deep-cases.py) is not set");
    QFETCH(QString, raw);
    QFETCH(QString, format);
    QFETCH(int, w);
    QFETCH(int, h);
    QFETCH(QString, layout);
    QFETCH(bool, first);
    QFETCH(QString, mark);
    QFETCH(QString, opts);
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    const std::map<QString, std::tuple<QVideoFrameFormat::PixelFormat, int, int, int, bool>> formats = {
        { u"YUV420P10"_s, { QVideoFrameFormat::Format_YUV420P10, 10, 0, 1, false } },
        { u"P010"_s, { QVideoFrameFormat::Format_P010, 10, 6, 1, true } },
        { u"P016"_s, { QVideoFrameFormat::Format_P016, 16, 0, 1, true } },
        { u"YUV422P16"_s, { QVideoFrameFormat::Format_YUV422P16, 16, 0, 0, false } },
        { u"YUV444P16"_s, { QVideoFrameFormat::Format_YUV444P16, 16, 0, -1, false } },
    };
    const auto [pf, bits, shift, subH, semi] = formats.at(format);
    const int subV = pf == QVideoFrameFormat::Format_YUV444P16 || pf == QVideoFrameFormat::Format_YUV422P16 ? 0 : 1;
    const int sub = pf == QVideoFrameFormat::Format_YUV444P16 ? 0 : 1; // chroma width shift
    Q_UNUSED(subH);
    QFile file(raw);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray data = file.readAll();
    const int cw = (w + (1 << sub) - 1) >> sub, ch = (h + (1 << subV) - 1) >> subV;
    // planar: Y, U, V; semi-planar: Y and the interleaved UV: 16 bit words
    const size_t ySize = size_t(w) * h * 2, cSize = size_t(cw) * ch * 2 * (semi ? 2 : 1);
    QVERIFY2(size_t(data.size()) >= ySize + cSize * (semi ? 1 : 2), "the raw frame is smaller than its format says");
    QVideoFrameFormat f(QSize(w, h), pf);
    f.setColorSpace(QVideoFrameFormat::ColorSpace_BT709);
    f.setColorRange(QVideoFrameFormat::ColorRange_Video);
    const bool tb = layout == u"ab"_s || layout == u"graphics"_s;
    const Mode mode = layout == u"sbs"_s ? Mode::DepthSideBySide : layout == u"ab"_s ? Mode::DepthTopBottom
            : layout == u"graphics"_s ? Mode::DepthGraphics : Mode::DepthBackground;
    Q_UNUSED(tb);
    f.setStereoMode(mode);
    QVideoFrame frame(f);
    QVERIFY(frame.map(QVideoFrame::WriteOnly));
    auto copyPlane = [&](int plane, const char *from, int rowBytes, int rows) {
        for (int y = 0; y < rows; ++y)
            std::memcpy(frame.bits(plane) + y * frame.bytesPerLine(plane), from + size_t(y) * rowBytes, size_t(rowBytes));
    };
    copyPlane(0, data.constData(), w * 2, h);
    if (semi) {
        copyPlane(1, data.constData() + ySize, cw * 4, ch);
    } else {
        copyPlane(1, data.constData() + ySize, cw * 2, ch);
        copyPlane(2, data.constData() + ySize + cSize, cw * 2, ch);
    }
    frame.unmap();

    auto o = fixedWay();
    o.automatic = false;
    o.mark = mark;
    for (const QString &opt : opts.split(u';', Qt::SkipEmptyParts)) {
        const QString k = opt.section(u'=', 0, 0), v = opt.section(u'=', 1);
        if (k == u"range"_s) o.range = v.toDouble();
        else if (k == u"zero"_s) o.zero = v.toDouble();
        else if (k == u"shift"_s) o.shift = v.toDouble();
        else if (k == u"view"_s) o.view = v.toInt();
        else if (k == u"warp"_s) o.warp = v.toInt();
        else if (k == u"fill"_s) o.fill = v.toInt();
        else if (k == u"smooth"_s) o.smooth = v.toInt();
        else if (k == u"sigmaH"_s) o.sigmaH = v.toDouble();
        else if (k == u"sigmaV"_s) o.sigmaV = v.toDouble();
        else if (k == u"radius"_s) o.radius = v.toInt();
    }
    QQuickVideoDepth::Renderer renderer;
    QVideoFrame cpu, gpu;
    const auto rc = renderer.draw(frame, mode, first, {}, o, &cpu);
    QVERIFY(!renderer.lastOnGpu());
    o.gpu = true;
    const auto rg = renderer.draw(frame, mode, first, {}, o, &gpu);
    if (rc == QQuickVideoDepth::Renderer::Result::PictureAlone) {
        QCOMPARE(rg, rc); // a malformed mark is one picture on either engine
        qunsetenv("XDG_CACHE_HOME");
        return;
    }
    QCOMPARE(rc, QQuickVideoDepth::Renderer::Result::Drawn);
    QCOMPARE(rg, QQuickVideoDepth::Renderer::Result::Drawn);
    if (!renderer.lastOnGpu())
        QSKIP("no OpenGL context in this environment");
    QCOMPARE(gpu.pixelFormat(), pf);
    QCOMPARE(gpu.size(), cpu.size());
    QVERIFY(gpu.map(QVideoFrame::ReadOnly));
    QVERIFY(cpu.map(QVideoFrame::ReadOnly));
    const double unit = double(1 << shift) * double(1 << (bits - 8));
    for (int plane = 0; plane < gpu.planeCount(); ++plane) {
        const int pw = plane == 0 ? gpu.width() : (((gpu.width() + (1 << sub) - 1) >> sub) * (semi ? 2 : 1));
        const int ph = plane == 0 ? gpu.height() : ((gpu.height() + (1 << subV) - 1) >> subV);
        long far = 0, over = 0;
        double sum = 0, maxd = 0;
        for (int y = 0; y < ph; ++y) {
            const quint16 *a = reinterpret_cast<const quint16 *>(gpu.bits(plane) + y * gpu.bytesPerLine(plane));
            const quint16 *b = reinterpret_cast<const quint16 *>(cpu.bits(plane) + y * cpu.bytesPerLine(plane));
            for (int x = 0; x < pw; ++x) {
                const double d = std::abs(int(a[x]) - int(b[x])) / unit;
                sum += d;
                maxd = std::max(maxd, d);
                far += d > 2.0;
                over += d > 0.0;
            }
        }
        const long px = long(pw) * ph;
        qInfo().noquote() << u"deepcase %1 plane %2: mean %3 max %4 (8 bit codes), off by more than 2: %5 of %6"_s
                                  .arg(raw.section(u'/', -1)).arg(plane).arg(sum / px, 0, 'f', 4).arg(maxd, 0, 'f', 2).arg(far).arg(px);
        // luma and 4:4:4 chroma: the 8 bit criterion. Subsampled chroma: the C code moves the chroma samples by whole chroma samples (the
        // parallax halved and rounded), the GPU moves the pixels of RGB and averages the block of a chroma sample, so the chroma at the edges
        // of the parallax differs by what the half resolution can hold: the mean stays under 0.3 codes, the pixels off by more than 2
        // (the edges) under 4 in 100
        // Telea's fill differs by its order (rings on the GPU, a heap in C): the library bounds the mean only (dibrgltest), so does this
        const bool telea = opts.contains(u"fill=2"_s);
        const bool subsampled = plane > 0 && (sub || subV);
        if (telea) {
            QVERIFY2(sum / px < 1.5, qPrintable(u"plane %1: mean difference %2 codes"_s.arg(plane).arg(sum / px)));
        } else if (!subsampled) {
            QVERIFY2(far * 500 < px, qPrintable(u"plane %1: %2 of %3 pixels off by more than 2 codes"_s.arg(plane).arg(far).arg(px)));
            QVERIFY2(sum / px < 0.25, qPrintable(u"plane %1: mean difference %2 codes"_s.arg(plane).arg(sum / px)));
        } else {
            QVERIFY2(far * 25 < px, qPrintable(u"plane %1: %2 of %3 pixels off by more than 2 codes"_s.arg(plane).arg(far).arg(px)));
            QVERIFY2(sum / px < 0.3, qPrintable(u"plane %1: mean difference %2 codes"_s.arg(plane).arg(sum / px)));
        }
    }
    gpu.unmap();
    cpu.unmap();
    qunsetenv("XDG_CACHE_HOME");
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

// A picture and its depth map side by side at 10 or 16 bits (YUV420P10, P010, P016): a gradient with the content's low bits in use
QVideoFrame deepDepthFrame(QVideoFrameFormat::PixelFormat pf, int w, int h)
{
    const bool semi = pf == QVideoFrameFormat::Format_P010 || pf == QVideoFrameFormat::Format_P016;
    const int shift = pf == QVideoFrameFormat::Format_P010 ? 6 : 0;
    const int scale = pf == QVideoFrameFormat::Format_P016 ? 64 : 1; // a 10 bit code as a 16 bit word
    QVideoFrameFormat format(QSize(2 * w, h), pf);
    format.setColorRange(QVideoFrameFormat::ColorRange_Full);
    format.setStereoMode(Mode::DepthSideBySide);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly))
        return {};
    for (int y = 0; y < h; ++y) {
        auto *row = reinterpret_cast<quint16 *>(frame.bits(0) + y * frame.bytesPerLine(0));
        for (int x = 0; x < 2 * w; ++x) {
            const int code = x < w ? 100 + (x * 6 + y * 4) % 800 : 200 + ((x - w) * 7 + y) % 600;
            row[x] = quint16((code * scale) << shift);
        }
    }
    const quint16 mid = quint16((512 * scale) << shift);
    for (int y = 0; y < h / 2; ++y) {
        if (semi) {
            auto *row = reinterpret_cast<quint16 *>(frame.bits(1) + y * frame.bytesPerLine(1));
            for (int x = 0; x < 2 * w; ++x)
                row[x] = mid;
        } else {
            for (int plane = 1; plane <= 2; ++plane) {
                auto *row = reinterpret_cast<quint16 *>(frame.bits(plane) + y * frame.bytesPerLine(plane));
                for (int x = 0; x < w; ++x)
                    row[x] = mid;
            }
        }
    }
    frame.unmap();
    return frame;
}

}

void tst_QQuickVideoStereo::depthViewsReachTheSinkAsTextures_data()
{
    QTest::addColumn<int>("format");
    QTest::newRow("8 bit, YUV 4:4:4") << int(QVideoFrameFormat::Format_YUV444P);
    QTest::newRow("10 bit, YUV 4:2:0") << int(QVideoFrameFormat::Format_YUV420P10);
    QTest::newRow("10 bit, P010") << int(QVideoFrameFormat::Format_P010);
    QTest::newRow("16 bit, P016") << int(QVideoFrameFormat::Format_P016);
}

// The drawn views reach the video sink as textures of the drawing context, shared with the scene graph's: no read back to system memory
// (viewsReadBacks 0), and what the window shows is what the read back (QT_MM_DEPTH_COPY_VIEWS) shows, pixel for pixel. The read back is
// the control: it is counted, and its reason is told.
void tst_QQuickVideoStereo::depthViewsReachTheSinkAsTextures()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    if (qgetenv("QSG_RHI_BACKEND") != "opengl")
        QSKIP("QSG_RHI_BACKEND=opengl is not set: the sink must draw with OpenGL");
    QFETCH(int, format);
    const auto pf = QVideoFrameFormat::PixelFormat(format);
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    const int w = 128, h = 64;
    struct Run
    {
        QImage image;
        QVariantMap route, counts;
    };
    auto run = [&](bool readBack) {
        Run r;
        qputenv("QT_MM_DEPTH_COPY_VIEWS", readBack ? "1" : "0");
        QQuickWindow window;
        window.resize(2 * w, h);
        QQuickVideoOutput output;
        output.setParentItem(window.contentItem());
        output.setSize(QSizeF(2 * w, h));
        window.show();
        if (!QTest::qWaitForWindowExposed(&window))
            return r;
        const QVideoFrame frame = pf == QVideoFrameFormat::Format_YUV444P ? depthFrame(Mode::DepthSideBySide, false, w, h)
                                                                          : deepDepthFrame(pf, w, h);
        for (int i = 0; i < 4; ++i) {
            output.videoSink()->setVideoFrame(frame);
            QTest::qWait(100);
        }
        r.route = output.stereoDepthRoute();
        r.counts = output.stereoDepthReadBacks();
        r.image = window.grabWindow().convertToFormat(QImage::Format_RGBA8888);
        return r;
    };
    const Run texture = run(false);
    const Run copy = run(true);
    qunsetenv("QT_MM_DEPTH_COPY_VIEWS");
    QVERIFY(!texture.image.isNull() && !copy.image.isNull());
    QCOMPARE(texture.route.value("views").toString(), u"texture"_s);
    QCOMPARE(texture.counts.value("views").toInt(), 0);
    QCOMPARE(copy.route.value("views").toString(), u"readBack"_s);
    QVERIFY(copy.counts.value("views").toInt() >= 1);
    QVERIFY(copy.route.value("viewsWhy").toString().contains(u"asked"_s));
    // the window shows something (not one flat colour), and the same with both routes
    std::set<QRgb> colours;
    for (int y = 0; y < texture.image.height(); y += 3)
        for (int x = 0; x < texture.image.width(); x += 3)
            colours.insert(texture.image.pixel(x, y));
    QVERIFY2(colours.size() > 16, "the window shows one flat colour");
    QCOMPARE(texture.image.size(), copy.image.size());
    int worst = 0;
    for (int y = 0; y < texture.image.height(); ++y)
        for (int x = 0; x < texture.image.width(); ++x) {
            const QRgb a = texture.image.pixel(x, y), b = copy.image.pixel(x, y);
            worst = std::max({ worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b)) });
        }
    QVERIFY2(worst == 0, qPrintable(u"the texture route and the read back differ by %1 codes"_s.arg(worst)));
}

void tst_QQuickVideoStereo::depthTextureFramesConvertLikeTheCopies_data()
{
    depthViewsReachTheSinkAsTextures_data();
}

// A consumer that is not the sink (the stereo layer's presenter converts on a thread of its own, with a context of its own) gets from a frame of
// textures what it gets from the frame the read back makes: the same image, whatever the depth of the source
void tst_QQuickVideoStereo::depthTextureFramesConvertLikeTheCopies()
{
    if (!QQuickVideoDepth::canDraw())
        QSKIP("built without libstereo-dibr");
    if (qgetenv("QSG_RHI_BACKEND") != "opengl")
        QSKIP("QSG_RHI_BACKEND=opengl is not set: the sink must draw with OpenGL");
    QFETCH(int, format);
    const auto pf = QVideoFrameFormat::PixelFormat(format);
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    const int w = 128, h = 64;
    QQuickWindow window;
    window.resize(64, 64);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QRhi *rhi = nullptr;
    QTRY_VERIFY((rhi = QQuickWindowPrivate::get(&window)->rhi) != nullptr);
    QVERIFY(rhi->backend() == QRhi::OpenGLES2);
    const QVideoFrame frame = pf == QVideoFrameFormat::Format_YUV444P ? depthFrame(Mode::DepthSideBySide, false, w, h)
                                                                      : deepDepthFrame(pf, w, h);
    auto drawn = [&](bool textures, QQuickVideoDepth::Renderer::ViewsRoute expected) {
        QQuickVideoDepth::Renderer renderer;
        renderer.setSinkRhi(rhi);
        QQuickVideoDepth::Options o = fixedWay();
        o.gpu = true;
        o.viewsAsTexture = textures;
        QVideoFrame out;
        for (int i = 0; i < 2; ++i)
            [&] { QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, o, &out), QQuickVideoDepth::Renderer::Result::Drawn); }();
        [&] { QVERIFY2(renderer.lastViewsRoute() == expected, qPrintable(renderer.lastViewsRouteWhy())); }();
        // converted on a thread of its own, as the presenter does
        QImage image;
        std::thread([&] { image = qImageFromVideoFrame(out, QVideoFrameImageDepth::Float16); }).join();
        return std::make_pair(image, renderer.viewsReadBacks());
    };
    const auto texture = drawn(true, QQuickVideoDepth::Renderer::ViewsRoute::Texture);
    const auto copy = drawn(false, QQuickVideoDepth::Renderer::ViewsRoute::ReadBack);
    QVERIFY(!texture.first.isNull());
    QVERIFY(!copy.first.isNull());
    QCOMPARE(texture.first.size(), copy.first.size());
    QCOMPARE(texture.first.format(), copy.first.format());
    // the consumer's mapping is a read back, and counted
    QVERIFY(texture.second >= 1);
    QCOMPARE(texture.first, copy.first);
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
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, fixedWay(), &pair),
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
    QCOMPARE(renderer.draw(frame, Mode::DepthSideBySide, false, {}, fixedWay(), &pair),
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
