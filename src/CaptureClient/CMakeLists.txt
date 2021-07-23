# Copyright (c) 2020 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

project(CaptureClient)
add_library(CaptureClient STATIC)

target_compile_options(CaptureClient PRIVATE ${STRICT_COMPILE_FLAGS})

target_include_directories(CaptureClient PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include)

target_include_directories(CaptureClient PRIVATE
        ${CMAKE_CURRENT_LIST_DIR})

target_sources(CaptureClient PUBLIC
        include/CaptureClient/ApiEventProcessor.h
        include/CaptureClient/CaptureClient.h
        include/CaptureClient/CaptureListener.h
        include/CaptureClient/CaptureEventProcessor.h
        include/CaptureClient/GpuQueueSubmissionProcessor.h)

target_sources(CaptureClient PRIVATE
        ApiEventProcessor.cpp
        CaptureClient.cpp
        CaptureEventProcessor.cpp
        CompositeEventProcessor.cpp
        GpuQueueSubmissionProcessor.cpp
        SaveToFileEventProcessor.cpp)

target_link_libraries(CaptureClient PUBLIC
        ApiInterface
        CaptureFile
        ClientData
        GrpcProtos
        Introspection
        CONAN_PKG::llvm-core)

add_fuzzer(CaptureEventProcessorProcessEventsFuzzer CaptureEventProcessorProcessEventsFuzzer.cpp)
target_link_libraries(
  CaptureEventProcessorProcessEventsFuzzer
  PRIVATE CaptureClient CONAN_PKG::libprotobuf-mutator)

add_executable(CaptureClientTests)

target_compile_options(CaptureClientTests PRIVATE ${STRICT_COMPILE_FLAGS})

target_sources(CaptureClientTests PRIVATE
        ApiEventProcessorTest.cpp
        CaptureEventProcessorTest.cpp
        CompositeEventProcessorTest.cpp
        SaveToFileEventProcessorTest.cpp)

target_link_libraries(
        CaptureClientTests PRIVATE
        CaptureClient
        GTest::Main)

register_test(CaptureClientTests)
