cmake_minimum_required(VERSION 3.25)
project(curl_multi_crasher LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
option(USE_INTERNAL_CURL "Build curl with threaded resolver (no c-ares)" ON)

if(USE_INTERNAL_CURL)
  # Declare the source and desired build options BEFORE processing.
  FetchContent_Declare(curl_src
    URL https://curl.se/download/curl-8.13.0.tar.xz
    URL_HASH SHA256=4a093979a3c2d02de2fbc00549a32771007f2e78032c6faa5ecd2f7a9e152025)

  # Set options that influence curl's own CMake when it is configured by
  # FetchContent. These must be cached *before* FetchContent_MakeAvailable.
  # Ensure DYLD symbol interposition works for all shared libs built in the
  # upcoming curl sub-project (must be set *before* FetchContent configures it)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(ENABLE_ARES OFF CACHE BOOL "" FORCE)
  set(ENABLE_THREADED_RESOLVER ON CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
  set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(curl_src)

  # FetchContent already defined target 'libcurl'; use it directly
  set(CURL_LIB_TARGET libcurl)
endif()

add_executable(crasher main.cpp)

# ---- fishhook static lib (local) ----
add_library(fishhook STATIC third_party/fishhook/fishhook.c)
target_include_directories(fishhook PUBLIC third_party/fishhook)
set_property(TARGET fishhook PROPERTY C_STANDARD 23)

# Objective-C++ interposer using fishhook
add_library(resolver_interpose SHARED fish_interposer.mm)
target_link_libraries(resolver_interpose PRIVATE fishhook)
set_target_properties(resolver_interpose PROPERTIES
  OUTPUT_NAME resolver_interpose
  CXX_STANDARD 23
  OBJCXX_STANDARD 23)

# Link against libcurl (threads come transitively)
set_property(TARGET crasher PROPERTY POSITION_INDEPENDENT_CODE ON)

# Link with selected curl target
target_link_libraries(crasher PRIVATE ${CURL_LIB_TARGET})
