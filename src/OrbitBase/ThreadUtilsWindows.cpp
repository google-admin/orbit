// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <Windows.h>
#include <processthreadsapi.h>

#include <string>

#include "OrbitBase/Logging.h"
#include "OrbitBase/ThreadUtils.h"

namespace orbit_base {

template <typename FunctionPrototypeT>
static FunctionPrototypeT GetProcAddress(const std::string& library, const std::string& procedure) {
  HMODULE module_handle = LoadLibraryA(library.c_str());
  if (module_handle == nullptr) {
    ERROR("Could not find procedure %s in %s", procedure, library);
    return nullptr;
  }
  return reinterpret_cast<FunctionPrototypeT>(::GetProcAddress(module_handle, procedure.c_str()));
}

pid_t GetCurrentThreadId() {
  thread_local pid_t current_tid = ::GetCurrentThreadId();
  return current_tid;
}

std::string GetThreadName(pid_t tid) {
  static const std::string kEmptyString;

  // Find "GetThreadDescription" procedure.
  static auto get_thread_description =
      GetProcAddress<HRESULT(WINAPI*)(HANDLE, PWSTR*)>("kernel32.dll", "GetThreadDescription");
  if (get_thread_description == nullptr) {
    ERROR("Getting thread name from id %u with proc[%llx]", tid, get_thread_description);
    return kEmptyString;
  }

  // Get thread handle from tid.
  HANDLE thread_handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (thread_handle == nullptr) {
    ERROR("Retrieving thread handle for tid %u", tid);
    return kEmptyString;
  }

  // Get thread name from handle.
  PWSTR thread_name_pwstr;
  if (SUCCEEDED((*get_thread_description)(thread_handle, &thread_name_pwstr))) {
    std::wstring thread_name_w(thread_name_pwstr);
    LocalFree(thread_name_pwstr);
    std::string thread_name(thread_name_w.begin(), thread_name_w.end());
    return thread_name;
  }

  ERROR("Getting thread name from id %u with proc[%llx]", tid, get_thread_description);
  return kEmptyString;
}

void SetCurrentThreadName(const char* name) {
  static auto set_thread_description =
      GetProcAddress<HRESULT(WINAPI*)(HANDLE, PCWSTR)>("kernel32.dll", "SetThreadDescription");
  std::wstring wide_name(name, name + strlen(name));
  if (set_thread_description == nullptr ||
      !SUCCEEDED((*set_thread_description)(GetCurrentThread(), wide_name.c_str()))) {
    ERROR("Setting thread name %s with proc[%llx]", name, set_thread_description);
  }
}

pid_t GetCurrentProcessId() { return ::GetCurrentProcessId(); }

}  // namespace orbit_base
