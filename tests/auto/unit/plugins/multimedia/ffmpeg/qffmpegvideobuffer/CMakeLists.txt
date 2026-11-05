# SPDX-License-Identifier: BSD-3-Clause
qt_internal_add_test(tst_qffmpegvideobuffer
    SOURCES tst_qffmpegvideobuffer.cpp
    LIBRARIES Qt::FFmpegMediaPluginImplPrivate Qt::MultimediaPrivate
)
