cmake_minimum_required(VERSION 3.24)
project(VoyagerArcade LANGUAGES C CXX OBJC OBJCXX)

# iOS / iPadOS and visionOS share the touch app (src/platform/ios).
set(VOY_MOBILE OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "visionOS")
  set(VOY_MOBILE ON)
endif()
# visionOS: Sense controller aiming is Swift (RealityKit, src/platform/visionos/aim.swift).
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
  set(voy_xros_min "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(NOT voy_xros_min)
    set(voy_xros_min 26.0)
  endif()
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(CMAKE_Swift_COMPILER_TARGET "arm64-apple-xros${voy_xros_min}-simulator")
  else()
    set(CMAKE_Swift_COMPILER_TARGET "arm64-apple-xros${voy_xros_min}")
  endif()
  enable_language(Swift)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The translated programs (tools/prepare.sh "<game folder>"). Derived from the
# player's files at build time; never committed.
set(VOY_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/x86tr" CACHE PATH "tools/prepare.sh output")
option(VOY_BUILD_MAC_APP "Build the Mac app" ON)
option(VOY_BUILD_TESTS "Build the tests" ON)
set(VOY_IOS_BUNDLE_ID "local.voyagerarcade.ios" CACHE STRING "iOS bundle identifier")

# The app icon, made from the player's own title screen by tools/make_icon.py
# (never committed). Without it the apps get the system's blank icon.
set(VOY_ICON_DIR "${CMAKE_SOURCE_DIR}/build/icon")
set(VOY_IOS_ICON_PLIST "")
if(EXISTS "${VOY_ICON_DIR}/Assets.xcassets")
  set(VOY_IOS_ICON_PLIST [=[    <key>CFBundleIcons</key>
    <dict><key>CFBundlePrimaryIcon</key><dict>
        <key>CFBundleIconFiles</key><array><string>AppIcon60x60</string></array>
        <key>CFBundleIconName</key><string>AppIcon</string></dict></dict>
    <key>CFBundleIcons~ipad</key>
    <dict><key>CFBundlePrimaryIcon</key><dict>
        <key>CFBundleIconFiles</key><array><string>AppIcon60x60</string><string>AppIcon76x76</string></array>
        <key>CFBundleIconName</key><string>AppIcon</string></dict></dict>
]=])
endif()
set(VOY_VISION_ICON_PLIST "")
if(EXISTS "${VOY_ICON_DIR}/AssetsVision.xcassets")
  set(VOY_VISION_ICON_PLIST [=[    <key>CFBundleIcons</key>
    <dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconName</key><string>AppIcon</string></dict></dict>
]=])
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VOY_ICON_DIR}")
set(VOY_IOS_TEAM "" CACHE STRING "Apple development team for iOS device builds")

# The manifest as a C++ string.
file(READ "${CMAKE_SOURCE_DIR}/manifests/voyager_e4.txt" VOY_MANIFEST)
file(WRITE "${CMAKE_BINARY_DIR}/generated/manifest.inc.tmp" "R\"VOYMANIFEST(${VOY_MANIFEST})VOYMANIFEST\"\n")
configure_file("${CMAKE_BINARY_DIR}/generated/manifest.inc.tmp" "${CMAKE_BINARY_DIR}/generated/manifest.inc" COPYONLY)

set(VOY_CORE_SOURCES
  src/recomp/dispatch.cpp
  src/recomp/guest.cpp
  src/recomp/guest_memory.cpp
  src/host/fail.cpp
  src/host/process.cpp
  src/host/os.cpp
  src/host/format.cpp
  src/host/libc.cpp
  src/host/stdio.cpp
  src/host/devices.cpp
  src/host/natives.cpp
  src/host/glut.cpp
  src/host/gl_imports.cpp
  src/host/machine.cpp
  src/gl/gl.cpp
  src/app/session.cpp
  src/decomp/decomp.cpp
  src/decomp/ps_math_3d.cpp
)
add_library(voy_core STATIC ${VOY_CORE_SOURCES})
target_include_directories(voy_core PUBLIC src "${CMAKE_BINARY_DIR}/generated")
target_compile_options(voy_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# One library per program from the generator's shard list.
set(VOY_GENERATED_LIBS)
foreach(prog trek stvsnd stvserv)
  set(list "${VOY_GENERATED_DIR}/${prog}/gen/sources.txt")
  if(NOT EXISTS "${list}")
    message(FATAL_ERROR "No translated ${prog}: run tools/prepare.sh \"<game folder>\" first (looked for ${list})")
  endif()
  file(STRINGS "${list}" names)
  set(srcs)
  foreach(n ${names})
    list(APPEND srcs "${VOY_GENERATED_DIR}/${prog}/gen/${n}")
  endforeach()
  add_library(voy_${prog} STATIC ${srcs})
  target_include_directories(voy_${prog} PRIVATE src)
  target_compile_options(voy_${prog} PRIVATE -O2 -w -fno-strict-aliasing)
  set_property(TARGET voy_${prog} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${list}")
  list(APPEND VOY_GENERATED_LIBS voy_${prog})
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${VOY_GENERATED_DIR}/trek/gen/sources.txt" "${VOY_GENERATED_DIR}/stvsnd/gen/sources.txt"
  "${VOY_GENERATED_DIR}/stvserv/gen/sources.txt" "${CMAKE_SOURCE_DIR}/manifests/voyager_e4.txt")

add_library(voy_game INTERFACE)
target_link_libraries(voy_game INTERFACE voy_core ${VOY_GENERATED_LIBS} voy_core)

if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/metal_renderer.mm")
  # The Metal renderer, shared by the Mac app, the iOS app and vgrun's frame dumps.
  add_library(voy_metal STATIC src/gpu/metal_renderer.mm)
  target_include_directories(voy_metal PUBLIC src)
  target_compile_options(voy_metal PRIVATE -fobjc-arc)
  target_link_libraries(voy_metal PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation"
                                         "-framework CoreGraphics" "-framework ImageIO")
endif()

if(NOT VOY_MOBILE)
  add_executable(vgrun tools/vgrun/main.cpp)
  target_link_libraries(vgrun PRIVATE voy_game)
  if(TARGET voy_metal)
    target_link_libraries(vgrun PRIVATE voy_metal)
  endif()
endif()

if(APPLE AND NOT VOY_MOBILE AND VOY_BUILD_MAC_APP AND EXISTS "${CMAKE_SOURCE_DIR}/src/platform/macos/main.mm")
  add_executable(VoyagerArcade MACOSX_BUNDLE
    src/platform/macos/main.mm
    src/platform/apple/game_host.mm
    src/platform/apple/audio.mm
    src/platform/apple/controllers.mm)
  target_compile_options(VoyagerArcade PRIVATE -fobjc-arc)
  target_link_libraries(VoyagerArcade PRIVATE voy_game voy_metal
    "-framework AppKit" "-framework MetalKit" "-framework AudioToolbox" "-framework GameController")
  set_target_properties(VoyagerArcade PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/platform/macos/Info.plist.in"
    OUTPUT_NAME "Voyager Arcade")
  if(EXISTS "${VOY_ICON_DIR}/VoyagerArcade.icns")
    target_sources(VoyagerArcade PRIVATE "${VOY_ICON_DIR}/VoyagerArcade.icns")
    set_source_files_properties("${VOY_ICON_DIR}/VoyagerArcade.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(VoyagerArcade PROPERTIES MACOSX_BUNDLE_ICON_FILE VoyagerArcade.icns)
  endif()
endif()

if(VOY_MOBILE)
  if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(voy_plist "${CMAKE_SOURCE_DIR}/src/platform/visionos/Info.plist.in")
    set(voy_family "7")
    set(voy_assets "${VOY_ICON_DIR}/AssetsVision.xcassets")
    set(voy_actool_os xros)
    set(voy_actool_devices --target-device vision)
    set(voy_actool_min 26.0)
  else()
    set(voy_plist "${CMAKE_SOURCE_DIR}/src/platform/ios/Info.plist.in")
    set(voy_family "1,2")
    set(voy_assets "${VOY_ICON_DIR}/Assets.xcassets")
    set(voy_actool_os iphoneos)
    set(voy_actool_devices --target-device iphone --target-device ipad)
    set(voy_actool_min 17.0)
  endif()
  add_executable(VoyagerArcadeIOS MACOSX_BUNDLE
    src/platform/ios/main.mm
    src/platform/apple/game_host.mm
    src/platform/apple/audio.mm
    src/platform/apple/controllers.mm)
  target_compile_options(VoyagerArcadeIOS PRIVATE -fobjc-arc)
  target_link_libraries(VoyagerArcadeIOS PRIVATE voy_game voy_metal
    "-framework UIKit" "-framework MetalKit" "-framework AudioToolbox" "-framework AVFAudio" "-framework GameController")
  set_target_properties(VoyagerArcadeIOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${voy_plist}"
    OUTPUT_NAME "VoyagerArcade"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/src/platform/ios/VoyagerArcade.entitlements"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${voy_family}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${VOY_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${VOY_IOS_BUNDLE_ID}")
  if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    # The Swift is a library of its own, away from the C++ flags swiftc cannot
    # take; clang links the app, and the Swift objects name the runtime
    # libraries they need, found in the SDK and the toolchain.
    add_library(voy_vision_swift STATIC src/platform/visionos/aim.swift)
    set_target_properties(voy_vision_swift PROPERTIES Swift_LANGUAGE_VERSION 6)
    target_link_libraries(VoyagerArcadeIOS PRIVATE voy_vision_swift
      "-framework RealityKit" "-framework SwiftUI" "-framework ARKit")
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
      set(voy_swift_platform xrsimulator)
    else()
      set(voy_swift_platform xros)
    endif()
    execute_process(COMMAND xcrun --find swiftc OUTPUT_VARIABLE voy_swiftc OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(voy_toolchain "${voy_swiftc}" DIRECTORY)
    get_filename_component(voy_toolchain "${voy_toolchain}" DIRECTORY)
    execute_process(COMMAND xcrun --sdk ${voy_swift_platform} --show-sdk-path
                    OUTPUT_VARIABLE voy_swift_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
    set_target_properties(VoyagerArcadeIOS PROPERTIES LINKER_LANGUAGE CXX)
    target_link_options(VoyagerArcadeIOS PRIVATE "-L${voy_swift_sdk}/usr/lib/swift"
                                                 "-L${voy_toolchain}/lib/swift/${voy_swift_platform}")
  endif()
  if(VOY_IOS_TEAM)
    set_target_properties(VoyagerArcadeIOS PROPERTIES
      XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${VOY_IOS_TEAM}"
      XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
  endif()
  if(EXISTS "${voy_assets}" AND CMAKE_GENERATOR STREQUAL "Xcode")
    # Xcode compiles the asset catalog itself.
    target_sources(VoyagerArcadeIOS PRIVATE "${voy_assets}")
    set_source_files_properties("${voy_assets}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(VoyagerArcadeIOS PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon)
  elseif(EXISTS "${voy_assets}")
    # Elsewhere actool writes Assets.car and the AppIcon images into the
    # bundle (before the ad hoc signing below); the Info.plist names them
    # (VOY_IOS_ICON_PLIST).
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
      string(REPLACE "os" "simulator" voy_actool_platform "${voy_actool_os}")  # iphoneos → iphonesimulator, xros → xrsimulator
    else()
      set(voy_actool_platform ${voy_actool_os})
    endif()
    add_custom_command(TARGET VoyagerArcadeIOS POST_BUILD
      COMMAND xcrun actool --compile $<TARGET_BUNDLE_DIR:VoyagerArcadeIOS> --platform ${voy_actool_platform}
              --minimum-deployment-target ${voy_actool_min} ${voy_actool_devices}
              --app-icon AppIcon --output-partial-info-plist ${CMAKE_CURRENT_BINARY_DIR}/actool-partial.plist
              --errors --warnings "${voy_assets}"
      VERBATIM)
  endif()
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator" AND NOT CMAKE_GENERATOR STREQUAL "Xcode")
    # The Simulator runs ad hoc signed apps.
    add_custom_command(TARGET VoyagerArcadeIOS POST_BUILD
      COMMAND codesign --force --sign - --timestamp=none $<TARGET_BUNDLE_DIR:VoyagerArcadeIOS>)
  endif()
endif()

if(VOY_BUILD_TESTS AND NOT VOY_MOBILE)
  enable_testing()
  add_subdirectory(tests)
endif()
