# Copyright (C) 2026 Daniel Campos Ramos
# SPDX-License-Identifier: BSD-3-Clause

#####################################################################
## tst_qquickvideostereo Test:
#####################################################################

qt_internal_add_test(tst_qquickvideostereo
    SOURCES
        tst_qquickvideostereo.cpp
    LIBRARIES
        Qt::Gui
        Qt::MultimediaPrivate
        Qt::MultimediaQuickPrivate
        Qt::Qml
        Qt::Quick
)
