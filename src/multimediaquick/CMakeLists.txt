# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

#####################################################################
## MultimediaQuickPrivate Module:
#####################################################################

set(qml_files
    "Video.qml"
)

qt_internal_add_qml_module(MultimediaQuickPrivate
    URI "QtMultimedia"
    VERSION "${PROJECT_VERSION}"
    PAST_MAJOR_VERSIONS 5
    CLASS_NAME QMultimediaQuickModule
    PLUGIN_TARGET quickmultimedia
    NO_GENERATE_PLUGIN_SOURCE
    NO_PLUGIN_OPTIONAL
    DEPENDENCIES QtQuick
    CONFIG_MODULE_NAME multimediaquick
    INTERNAL_MODULE
    SOURCES
        qquickimagecapture.cpp qquickimagecapture_p.h
        qquickimagepreviewprovider.cpp qquickimagepreviewprovider_p.h
        qquickmediaplayer.cpp qquickmediaplayer_p.h
        qquickscreencapture.cpp qquickscreencapture_p.h
        qquicksoundeffect.cpp qquicksoundeffect_p.h
        qquickvideooutput.cpp qquickvideooutput_p.h
        qquickvideostereo.cpp qquickvideostereo_p.h
        qquickvideostereolayer.cpp qquickvideostereolayer_p.h
        qquickvideostereopresenter.cpp qquickvideostereopresenter_p.h
        qsgvideonode_p.cpp qsgvideonode_p.h
        qsgvideotexture.cpp qsgvideotexture_p.h
        qtmultimediaquickglobal_p.h
        qtmultimediaquicktypes.cpp qtmultimediaquicktypes_p.h
    DEFINES
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
    QML_FILES
        ${qml_files}
    PUBLIC_LIBRARIES
        Qt::Core
        Qt::Multimedia
        Qt::MultimediaPrivate
        Qt::Quick
        Qt::QuickPrivate
)

target_sources(quickmultimedia PRIVATE multimedia_plugin.cpp)

# The 3D area of a video in a surface of its own under the window's (Wayland subsurface),
# declared to the compositor as stereo content: only where the Wayland client library, the
# protocols and the declaration library are there to build against
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
    pkg_check_modules(WAYLAND_SCANNER_PC QUIET wayland-scanner)
    pkg_check_modules(STEREO_DECLARE QUIET IMPORTED_TARGET stereo-declare)
endif()
if(NOT QT_NO_STEREO_LAYER AND WAYLAND_CLIENT_FOUND AND WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER_PC_FOUND AND STEREO_DECLARE_FOUND)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
    set(stereo_layer_protocols
        "stable/viewporter/viewporter.xml:viewporter"
        "staging/color-management/color-management-v1.xml:color-management-v1")
    set(stereo_layer_sources)
    set(stereo_layer_headers)
    foreach(entry IN LISTS stereo_layer_protocols)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 xml)
        list(GET parts 1 base)
        set(header "${CMAKE_CURRENT_BINARY_DIR}/wayland-${base}-client-protocol.h")
        set(code "${CMAKE_CURRENT_BINARY_DIR}/wayland-${base}-protocol.c")
        add_custom_command(OUTPUT "${header}" "${code}"
            COMMAND "${WAYLAND_SCANNER}" client-header "${WAYLAND_PROTOCOLS_DIR}/${xml}" "${header}"
            COMMAND "${WAYLAND_SCANNER}" private-code "${WAYLAND_PROTOCOLS_DIR}/${xml}" "${code}"
            DEPENDS "${WAYLAND_PROTOCOLS_DIR}/${xml}"
            VERBATIM)
        list(APPEND stereo_layer_sources "${code}")
        list(APPEND stereo_layer_headers "${header}")
    endforeach()
    # the generated headers are not sources of the module (the module's headers are installed)
    add_custom_target(MultimediaQuickStereoLayerProtocols DEPENDS ${stereo_layer_headers})
    add_dependencies(MultimediaQuickPrivate MultimediaQuickStereoLayerProtocols)
    target_sources(MultimediaQuickPrivate PRIVATE ${stereo_layer_sources})
    target_include_directories(MultimediaQuickPrivate PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(MultimediaQuickPrivate PRIVATE QT_MM_STEREO_LAYER)
    target_link_libraries(MultimediaQuickPrivate PRIVATE Qt::GuiPrivate
        PkgConfig::WAYLAND_CLIENT PkgConfig::STEREO_DECLARE)
endif()
