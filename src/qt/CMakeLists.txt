# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# See:
#  - https://cmake.org/cmake/help/latest/manual/cmake-qt.7.html
#  - https://doc.qt.io/qt-5/cmake-manual.html

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/forms)

set(qt_package ${WITH_GUI})

# The PATH_SUFFIXES option is required on OpenBSD systems.
find_package(${qt_package} 5.11.3 REQUIRED
  COMPONENTS Widgets Network LinguistTools
  HINTS ${qt5_brew_prefix}
  PATH_SUFFIXES Qt5
)
message(STATUS "Found Qt, version ${${qt_package}_VERSION}")

file(GLOB ts_files RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} locale/*.ts)
set_source_files_properties(${ts_files} PROPERTIES OUTPUT_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/locale)
qt5_add_translation(qm_files ${ts_files})

add_library(bitcoinqt STATIC EXCLUDE_FROM_ALL "")
target_sources(bitcoinqt
  PRIVATE
    bantablemodel.cpp
    bitcoin.cpp
    bitcoinaddressvalidator.cpp
    bitcoinamountfield.cpp
    bitcoingui.cpp
    bitcoinunits.cpp
    clientmodel.cpp
    csvmodelwriter.cpp
    guiutil.cpp
    intro.cpp
    modaloverlay.cpp
    networkstyle.cpp
    notificator.cpp
    optionsdialog.cpp
    optionsmodel.cpp
    peertablemodel.cpp
    qvalidatedlineedit.cpp
    qvaluecombobox.cpp
    rpcconsole.cpp
    splashscreen.cpp
    trafficgraphwidget.cpp
    utilitydialog.cpp
    $<$<PLATFORM_ID:Windows>:winshutdownmonitor.cpp>
    $<$<PLATFORM_ID:Darwin>:macdockiconhandler.mm>
    $<$<PLATFORM_ID:Darwin>:macnotificationhandler.mm>
    $<$<PLATFORM_ID:Darwin>:macos_appnap.mm>
)
target_include_directories(bitcoinqt
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)
target_link_libraries(bitcoinqt
  PUBLIC
    ${qt_package}::Widgets
  PRIVATE
    rpc_client
    leveldb
    Boost::headers
    $<$<CXX_COMPILER_ID:MSVC>:shlwapi>
)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_libraries(bitcoinqt PRIVATE "-framework AppKit")
endif()
if(TARGET NATPMP::NATPMP)
  target_link_libraries(bitcoinqt PRIVATE NATPMP::NATPMP)
endif()
if(TARGET MiniUPnPc::MiniUPnPc)
  target_link_libraries(bitcoinqt PRIVATE MiniUPnPc::MiniUPnPc)
endif()
if(TARGET PkgConfig::libqrencode)
  target_link_libraries(bitcoinqt PRIVATE PkgConfig::libqrencode)
endif()

if(ENABLE_WALLET)
  target_sources(bitcoinqt
    PRIVATE
      addressbookpage.cpp
      addresstablemodel.cpp
      askpassphrasedialog.cpp
      coincontroldialog.cpp
      coincontroltreewidget.cpp
      createwalletdialog.cpp
      editaddressdialog.cpp
      openuridialog.cpp
      overviewpage.cpp
      paymentserver.cpp
      qrimagewidget.cpp
      receivecoinsdialog.cpp
      receiverequestdialog.cpp
      recentrequeststablemodel.cpp
      sendcoinsdialog.cpp
      sendcoinsentry.cpp
      signverifymessagedialog.cpp
      transactiondesc.cpp
      transactiondescdialog.cpp
      transactionfilterproxy.cpp
      transactionrecord.cpp
      transactiontablemodel.cpp
      transactionview.cpp
      walletcontroller.cpp
      walletframe.cpp
      walletmodel.cpp
      walletmodeltransaction.cpp
      walletview.cpp
  )
  target_link_libraries(bitcoinqt
    PRIVATE
      bitcoin_wallet
      ${qt_package}::Network
  )
endif()

if(CMAKE_CROSSCOMPILING)
  target_compile_definitions(bitcoinqt PRIVATE QT_STATICPLUGIN)
  set_property(
    TARGET Qt5::Core
    APPEND
    PROPERTY INTERFACE_LINK_LIBRARIES
      ${QT_CROSS_LIBS_PATH}/libqtpcre2.a
      ${QT_CROSS_LIBS_PATH}/libqtlibpng.a
      ${QT_CROSS_LIBS_PATH}/libqtharfbuzz.a
      $<$<PLATFORM_ID:Linux>:dl>
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET Qt5::QXcbIntegrationPlugin)
    target_compile_definitions(bitcoinqt PRIVATE QT_QPA_PLATFORM_XCB)
    set_property(
      TARGET Qt5::Core
      APPEND
      PROPERTY INTERFACE_LINK_LIBRARIES
        -L${QT_CROSS_LIBS_PATH}
        freetype
        fontconfig
        xcb
        xcb-icccm
        xcb-image
        xcb-keysyms
        xcb-randr
        xcb-render
        xcb-render-util
        xcb-shape
        xcb-shm
        xcb-sync
        xcb-xfixes
        xcb-xinerama
        xcb-xkb
        xkbcommon
        xkbcommon-x11
    )
  elseif(WIN32 AND TARGET Qt5::QWindowsIntegrationPlugin AND TARGET Qt5::QWindowsVistaStylePlugin)
    target_compile_definitions(bitcoinqt PRIVATE QT_QPA_PLATFORM_WINDOWS)
    target_link_libraries(Qt5::Core
      INTERFACE
        dwmapi
        imm32
        netapi32
        shlwapi
        userenv
        uxtheme
        version
        winmm
        wtsapi32
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET Qt5::QCocoaIntegrationPlugin AND TARGET Qt5::QMacStylePlugin)
    target_compile_definitions(bitcoinqt PRIVATE QT_QPA_PLATFORM_COCOA)
    target_link_libraries(Qt5::Core
      INTERFACE
        "-framework AppKit"
        "-framework Carbon"
        "-framework CoreVideo"
        "-framework IOKit"
        "-framework IOSurface"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Security"
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL Android AND TARGET Qt5::QAndroidPlatformIntegrationPlugin)
    target_compile_definitions(bitcoinqt PRIVATE QT_QPA_PLATFORM_ANDROID)
  endif()
endif()

add_executable(bitcoin-qt "")
target_sources(bitcoin-qt
  PRIVATE
    main.cpp
)
target_link_libraries(bitcoin-qt
  PRIVATE
    bitcoinqt
    bitcoin_node
)
