cmake_minimum_required(VERSION 3.25)
project(PulseApkInfo LANGUAGES C CXX)
# The Pulse APK Info extension. Build in an x64 MSVC environment:
#   cmake -S extensions/apkinfo -B build-extensions/apkinfo -G Ninja
#   cmake --build build-extensions/apkinfo
# Static CRT like the host (built with MultiThreaded): the package DLL must
# load on machines without the VC++ redistributable, and the plugin ABI is a
# plain C surface, so no CRT state crosses the boundary.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(PULSE_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../sdk/native/include" CACHE PATH "Public SDK headers")

file(GLOB PULSE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PULSE_SDK_INCLUDE}/pulse/*.h" "${PULSE_SDK_INCLUDE}/pulse/*.hpp")
set(PLUGIN_SOURCES plugin.cpp apk_info.cpp zip_reader.cpp axml_parser.cpp arsc_parser.cpp info_model.cpp text.cpp)

add_library(miniz_vendor STATIC third_party/miniz/miniz.c)
target_include_directories(miniz_vendor PUBLIC third_party/miniz)
# Vendored third-party C source does not target the project's /W4 policy.
if(MSVC)
    set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "/W3")
    target_compile_definitions(miniz_vendor PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(pulse_apkinfo SHARED ${PLUGIN_SOURCES})
set_source_files_properties(${PLUGIN_SOURCES} PROPERTIES OBJECT_DEPENDS "${PULSE_PUBLIC_HEADERS}")
target_include_directories(pulse_apkinfo PRIVATE "${PULSE_SDK_INCLUDE}")
target_compile_features(pulse_apkinfo PRIVATE cxx_std_20)
target_compile_options(pulse_apkinfo PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_apkinfo PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulse_apkinfo PRIVATE miniz_vendor)

option(PULSE_APKINFO_TESTS "Build the APK info parser tests" ON)
if(PULSE_APKINFO_TESTS)
    add_executable(pulse_apkinfo_test apkinfo_test.cpp axml_parser.cpp arsc_parser.cpp zip_reader.cpp apk_info.cpp info_model.cpp text.cpp)
    set_source_files_properties(apkinfo_test.cpp apk_info.cpp zip_reader.cpp axml_parser.cpp arsc_parser.cpp info_model.cpp text.cpp
        PROPERTIES OBJECT_DEPENDS "${PULSE_PUBLIC_HEADERS}")
    target_include_directories(pulse_apkinfo_test PRIVATE "${PULSE_SDK_INCLUDE}")
    target_compile_features(pulse_apkinfo_test PRIVATE cxx_std_20)
    target_compile_options(pulse_apkinfo_test PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(pulse_apkinfo_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(pulse_apkinfo_test PRIVATE miniz_vendor)
endif()
