// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_BASE_THREAD_UTILS_H_
#define ORBIT_BASE_THREAD_UTILS_H_

#ifdef _WIN32
#include <stdint.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include <string>

#ifdef _WIN32
// We define pid_t on Windows to ease cross-platform development. On Windows, GetCurrentThreadId()
// and GetCurrentProcessId() both return a DWORD, an unsigned 32-bit type. Note that on Linux, pid_t
// is a signed 32-bit type. This type aliasing matches that of absl/base/internal/sysinfo.h.
using pid_t = uint32_t;
#endif

namespace orbit_base {

[[nodiscard]] pid_t GetCurrentThreadId();
[[nodiscard]] std::string GetThreadName(pid_t tid);
[[nodiscard]] pid_t GetCurrentProcessId();

void SetCurrentThreadName(const char* thread_name);

}  // namespace orbit_base

#endif  // ORBIT_BASE_THREAD_UTILS_H_
