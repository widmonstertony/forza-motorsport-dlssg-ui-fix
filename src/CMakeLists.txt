cmake_minimum_required(VERSION 3.27)
project(forza_motorsport_dlssg_ui_fix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(RESHade_SOURCE_DIR "" CACHE PATH "Path to a ReShade source checkout")
set(STREAMLINE_SOURCE_DIR "" CACHE PATH "Path to NVIDIA Streamline source")
set(DETOURS_SOURCE_DIR "" CACHE PATH "Path to Microsoft Detours src directory")

foreach(required RESHade_SOURCE_DIR STREAMLINE_SOURCE_DIR DETOURS_SOURCE_DIR)
  if(NOT ${required})
    message(FATAL_ERROR "${required} must be specified")
  endif()
endforeach()

add_library(fm_detours STATIC
  "${DETOURS_SOURCE_DIR}/detours.cpp"
  "${DETOURS_SOURCE_DIR}/modules.cpp"
  "${DETOURS_SOURCE_DIR}/disasm.cpp"
  "${DETOURS_SOURCE_DIR}/image.cpp"
  "${DETOURS_SOURCE_DIR}/creatwth.cpp"
  "${DETOURS_SOURCE_DIR}/disolx86.cpp"
  "${DETOURS_SOURCE_DIR}/disolx64.cpp"
  "${DETOURS_SOURCE_DIR}/disolia64.cpp"
  "${DETOURS_SOURCE_DIR}/disolarm.cpp"
  "${DETOURS_SOURCE_DIR}/disolarm64.cpp")
target_compile_definitions(fm_detours PRIVATE WIN32_LEAN_AND_MEAN)
set_target_properties(fm_detours PROPERTIES CXX_STANDARD 14)

add_library(fm_hudless_mfg MODULE fm-hudless-mfg/addon.cpp)
set_target_properties(fm_hudless_mfg PROPERTIES
  PREFIX "" OUTPUT_NAME "fm-hudless-mfg" SUFFIX ".addon64")
target_compile_definitions(fm_hudless_mfg PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fm_hudless_mfg PRIVATE /bigobj /utf-8)
target_include_directories(fm_hudless_mfg SYSTEM PRIVATE
  "${STREAMLINE_SOURCE_DIR}/include"
  "${STREAMLINE_SOURCE_DIR}/external/json/include"
  "${STREAMLINE_SOURCE_DIR}/external/ngx-sdk/include"
  "${RESHade_SOURCE_DIR}"
  "${DETOURS_SOURCE_DIR}")
target_link_libraries(fm_hudless_mfg PRIVATE fm_detours version d3d12 dxgi)

