// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only
#include <QtTest/qtest.h>
#include <QtTest/qsignalspy.h>
#include <QtMultimedia/qvideosink.h>
#include <QtFFmpegMediaPluginImpl/private/qffmpegvideobuffer_p.h>
extern "C" {
#include <libavutil/stereo3d.h>
}

class tst_QFFmpegVideoBuffer : public QObject
{
    Q_OBJECT
private slots:
    void nativeSize_data();
    void nativeSize();
    void sinkProperty();
};

void tst_QFFmpegVideoBuffer::nativeSize_data()
{
    QTest::addColumn<bool>("preserve");
    QTest::addColumn<int>("mark");
    QTest::addColumn<bool>("inverted");
    QTest::addColumn<int>("sar");
    for (bool preserve : { false, true }) {
        for (int mark : { -1, int(AV_STEREO3D_2D), int(AV_STEREO3D_SIDEBYSIDE),
                          int(AV_STEREO3D_TOPBOTTOM), int(AV_STEREO3D_LINES) }) {
            for (bool inverted : { false, true }) {
                for (int sar : { 1, 2 }) {
                    const auto name = QByteArray::number(preserve) + "-" + QByteArray::number(mark)
                            + "-" + QByteArray::number(inverted) + "-" + QByteArray::number(sar);
                    QTest::newRow(name.constData()) << preserve << mark << inverted << sar;
                }
            }
        }
    }
}

void tst_QFFmpegVideoBuffer::nativeSize()
{
    QFETCH(bool, preserve);
    QFETCH(int, mark);
    QFETCH(bool, inverted);
    QFETCH(int, sar);
    auto frame = QFFmpeg::makeAVFrame();
    frame->width = 64;
    frame->height = 32;
    frame->format = AV_PIX_FMT_BGRA;
    QCOMPARE(av_frame_get_buffer(frame.get(), 0), 0);
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            auto *p = frame->data[0] + y * frame->linesize[0] + x * 4;
            p[0] = x;
            p[1] = y;
            p[2] = x >= 32 ? 192 : 64;
            p[3] = 255;
        }
    }
    if (mark >= 0) {
        auto *stereo = av_stereo3d_create_side_data(frame.get());
        QVERIFY(stereo);
        stereo->type = AVStereo3DType(mark);
        stereo->flags = inverted ? AV_STEREO3D_FLAG_INVERT : 0;
    }
    QFFmpegVideoBuffer buffer(std::move(frame), { sar, 1 },
                             QVideoFrameFormat::StereoMode::None, preserve);
    const bool native = preserve || sar == 1 || mark == AV_STEREO3D_SIDEBYSIDE
            || mark == AV_STEREO3D_TOPBOTTOM;
    QCOMPARE(buffer.size(), QSize(native ? 64 : 128, 32));
    QCOMPARE(buffer.pixelAspectRatio(), QSize(native ? sar : 1, 1));
    QCOMPARE(buffer.stereo().has_value(), mark >= 0);
    if (mark >= 0) {
        QCOMPARE(buffer.stereo()->flags.testFlag(QVideoFrameFormat::StereoFlag::RightEyeFirst),
                 inverted);
        if (mark == AV_STEREO3D_2D)
            QCOMPARE(buffer.stereo()->mode, QVideoFrameFormat::StereoMode::None);
    }
    const auto data = buffer.map(QVideoFrame::ReadOnly);
    QVERIFY(data.data[0]);
    if (native) {
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto *p = data.data[0] + y * data.bytesPerLine[0] + x * 4;
                QCOMPARE(p[0], x);
                QCOMPARE(p[1], y);
                QCOMPARE(p[2], x >= 32 ? 192 : 64);
                QCOMPARE(p[3], 255);
            }
        }
    }
    buffer.unmap();
}

void tst_QFFmpegVideoBuffer::sinkProperty()
{
    QVideoSink sink;
    QVERIFY(!sink.preserveNativeSize());
    QSignalSpy changed(&sink, &QVideoSink::preserveNativeSizeChanged);
    sink.setPreserveNativeSize(true);
    QVERIFY(sink.preserveNativeSize());
    QCOMPARE(changed.size(), 1);
    sink.setPreserveNativeSize(true);
    QCOMPARE(changed.size(), 1);
    sink.setPreserveNativeSize(false);
    QVERIFY(!sink.preserveNativeSize());
    QCOMPARE(changed.size(), 2);
}

QTEST_GUILESS_MAIN(tst_QFFmpegVideoBuffer)
#include "tst_qffmpegvideobuffer.moc"
