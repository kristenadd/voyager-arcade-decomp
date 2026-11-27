cmake_minimum_required(VERSION 3.24)
project(VoyagerArcade LANGUAGES C CXX OBJC OBJCXX)

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

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  add_executable(vgrun tools/vgrun/main.cpp)
  target_link_libraries(vgrun PRIVATE voy_game)
  if(TARGET voy_metal)
    target_link_libraries(vgrun PRIVATE voy_metal)
  endif()
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND VOY_BUILD_MAC_APP AND EXISTS "${CMAKE_SOURCE_DIR}/src/platform/macos/main.mm")
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
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  add_executable(VoyagerArcadeIOS MACOSX_BUNDLE
    src/platform/ios/main.mm
    src/platform/apple/game_host.mm
    src/platform/apple/audio.mm
    src/platform/apple/controllers.mm)
  target_compile_options(VoyagerArcadeIOS PRIVATE -fobjc-arc)
  target_link_libraries(VoyagerArcadeIOS PRIVATE voy_game voy_metal
    "-framework UIKit" "-framework MetalKit" "-framework AudioToolbox" "-framework AVFAudio" "-framework GameController")
  set_target_properties(VoyagerArcadeIOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/platform/ios/Info.plist.in"
    OUTPUT_NAME "VoyagerArcade"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/src/platform/ios/VoyagerArcade.entitlements"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
endif()

if(VOY_BUILD_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  enable_testing()
  add_subdirectory(tests)
endif()
