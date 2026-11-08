#-----------------------------------------------------------------------------------------------#
# OpenRGB 1.1 plugin for the Aukey KM-P6 RGB mouse pad (Linux)                                  #
#                                                                                               #
# Build against the same Qt major version as the OpenRGB you load it into (Qt 6).               #
#-----------------------------------------------------------------------------------------------#

QT          += core gui widgets
TEMPLATE     = lib
CONFIG      += plugin c++17 silent
TARGET       = OpenRGB-KMP6

isEmpty(OPENRGB_SOURCE): OPENRGB_SOURCE = $$PWD/OpenRGB

INCLUDEPATH +=                                                                                  \
    src                                                                                         \
    $$OPENRGB_SOURCE                                                                            \
    $$OPENRGB_SOURCE/RGBController                                                              \
    $$OPENRGB_SOURCE/dependencies/json                                                          \

HEADERS +=                                                                                      \
    src/UsbTransport.h                                                                          \
    src/KMP6Protocol.h                                                                          \
    src/KMP6Plugin.h                                                                            \

SOURCES +=                                                                                      \
    src/KMP6Protocol.cpp                                                                        \
    src/KMP6Plugin.cpp                                                                          \

DISTFILES += KMP6Plugin.json

unix:!macx {
    SOURCES         += src/UsbTransport_Linux.cpp
    target.path      = $$PREFIX/lib/openrgb/plugins
    INSTALLS        += target
}

!unix|macx {
    error("The KM-P6 plugin is for Linux only; on Windows OpenRGB drives the pad itself (Holtek Mousemat).")
}
