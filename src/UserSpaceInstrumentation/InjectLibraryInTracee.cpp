// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "UserSpaceInstrumentation/InjectLibraryInTracee.h"

#include <absl/base/casts.h>
#include <absl/strings/str_format.h>

#include <cstring>
#include <string>
#include <vector>

#include "AccessTraceesMemory.h"
#include "AllocateInTracee.h"
#include "ExecuteMachineCode.h"
#include "FindFunctionAddress.h"
#include "MachineCode.h"
#include "OrbitBase/File.h"
#include "OrbitBase/UniqueResource.h"

namespace orbit_user_space_instrumentation {

namespace {

// Size of the small amount of memory we need in the tracee to write machine code into.
constexpr uint64_t kCodeScratchPadSize = 1024;

constexpr const char* kLibcSoname = "libc.so.6";
constexpr const char* kLibdlSoname = "libdl.so.2";
constexpr const char* kDlopenInLibdl = "dlopen";
constexpr const char* kDlopenInLibc = "__libc_dlopen_mode";
constexpr const char* kDlsymInLibdl = "dlsym";
constexpr const char* kDlsymInLibc = "__libc_dlsym";
constexpr const char* kDlcloseInLibdl = "dlclose";
constexpr const char* kDlcloseInLibc = "__libc_dlclose";

// Returns the absolute virtual address of a function in a module of a process as
// FindFunctionAddress does but accepts a fallback symbol if the primary one cannot be resolved.
ErrorMessageOr<uint64_t> FindFunctionAddressWithFallback(pid_t pid, std::string_view module,
                                                         std::string_view function,
                                                         std::string_view fallback_module,
                                                         std::string_view fallback_function) {
  ErrorMessageOr<uint64_t> primary_address_or_error = FindFunctionAddress(pid, module, function);
  if (primary_address_or_error.has_value()) {
    return primary_address_or_error.value();
  }
  ErrorMessageOr<uint64_t> fallback_address_or_error =
      FindFunctionAddress(pid, fallback_module, fallback_function);
  if (fallback_address_or_error.has_value()) {
    return fallback_address_or_error.value();
  }

  return ErrorMessage(absl::StrFormat(
      "Failed to load symbol \"%s\" from module \"%s\" with error: \"%s\"\nAnd also "
      "failed to load fallback symbol \"%s\" from module \"%s\" with error: \"%s\"",
      function, module, primary_address_or_error.error().message(), fallback_function,
      fallback_module, fallback_address_or_error.error().message()));
}

}  // namespace

[[nodiscard]] ErrorMessageOr<void*> DlopenInTracee(pid_t pid, std::filesystem::path path,
                                                   uint32_t flag) {
  // Make sure file exists.
  auto file_exists_or_error = orbit_base::FileExists(path);
  if (file_exists_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat("Unable to access library at: \"%s\": %s", path,
                                        file_exists_or_error.error().message()));
  }
  if (file_exists_or_error.value() == false) {
    return ErrorMessage(absl::StrFormat("Library does not exist at: \"%s\"", path));
  }

  // Figure out address of dlopen.
  OUTCOME_TRY(dlopen_address, FindFunctionAddressWithFallback(pid, kLibdlSoname, kDlopenInLibdl,
                                                              kLibcSoname, kDlopenInLibc));

  // Allocate small memory area in the tracee. This is used for the code and the path name.
  const uint64_t path_length = path.string().length() + 1;  // Include terminating zero.
  const uint64_t memory_size = kCodeScratchPadSize + path_length;
  auto code_memory_or_error = AllocateInTraceeAsUniqueResource(pid, 0, memory_size);
  if (code_memory_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to allocate memory in tracee: %s",
                                        code_memory_or_error.error().message()));
  }
  auto code_memory = std::move(code_memory_or_error.value());

  // Write the name of the .so into memory at code_memory with offset of kCodeScratchPadSize.
  std::vector<uint8_t> path_as_vector(path_length);
  memcpy(path_as_vector.data(), path.c_str(), path_length);
  const uint64_t so_path_address = code_memory.get_mutable().GetAddress() + kCodeScratchPadSize;
  auto write_memory_result = WriteTraceesMemory(pid, so_path_address, path_as_vector);
  if (write_memory_result.has_error()) {
    return write_memory_result.error();
  }

  // We want to do the following in the tracee:
  // return_value = dlopen(path, flag);
  // The calling convention is to put the parameters in registers rdi and rsi.
  // So the address of the file path goes to rdi. The flag argument goes into rsi. Then we load the
  // address of dlopen into rax and do the call. Assembly in Intel syntax (destination first),
  // machine code on the right:

  // movabsq rdi, so_path_address     48 bf so_path_address
  // movl esi, flag                   be flag
  // movabsq rax, dlopen_address      48 b8 dlopen_address
  // call rax                         ff d0
  // int3                             cc
  MachineCode code;
  code.AppendBytes({0x48, 0xbf})
      .AppendImmediate64(so_path_address)
      .AppendBytes({0xbe})
      .AppendImmediate32(flag)
      .AppendBytes({0x48, 0xb8})
      .AppendImmediate64(dlopen_address)
      .AppendBytes({0xff, 0xd0})
      .AppendBytes({0xcc});

  OUTCOME_TRY(return_value, ExecuteMachineCode(code_memory.get_mutable(), code));

  return absl::bit_cast<void*>(return_value);
}

[[nodiscard]] ErrorMessageOr<void*> DlsymInTracee(pid_t pid, void* handle,
                                                  std::string_view symbol) {
  // Figure out address of dlsym.
  OUTCOME_TRY(dlsym_address, FindFunctionAddressWithFallback(pid, kLibdlSoname, kDlsymInLibdl,
                                                             kLibcSoname, kDlsymInLibc));

  // Allocate small memory area in the tracee. This is used for the code and the symbol name.
  const size_t symbol_name_length = symbol.length() + 1;  // include terminating zero
  const uint64_t memory_size = kCodeScratchPadSize + symbol_name_length;
  auto code_memory_or_error = AllocateInTraceeAsUniqueResource(pid, 0, memory_size);
  if (code_memory_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to allocate memory in tracee: %s",
                                        code_memory_or_error.error().message()));
  }
  auto code_memory = std::move(code_memory_or_error.value());

  // Write the name of symbol into memory at code_memory with offset of kCodeScratchPadSize.
  std::vector<uint8_t> symbol_name_as_vector(symbol_name_length, 0);
  memcpy(symbol_name_as_vector.data(), symbol.data(), symbol.length());
  const uint64_t symbol_name_address = code_memory.get_mutable().GetAddress() + kCodeScratchPadSize;
  auto write_memory_result = WriteTraceesMemory(pid, symbol_name_address, symbol_name_as_vector);
  if (write_memory_result.has_error()) {
    return write_memory_result.error();
  }

  // We want to do the following in the tracee:
  // return_value = dlsym(handle, symbol);
  // The calling convention is to put the parameters in registers rdi and rsi.
  // So the handle goes to rdi and the address of the symbol name goes to rsi. Then we load the
  // address of dlsym into rax and do the call. Assembly in Intel syntax (destination first),
  // machine code on the right:

  // movabsq rdi, handle              48 bf handle
  // movabsq rsi, symbol_name_address 48 be symbol_name_address
  // movabsq rax, dlsym_address       48 b8 dlsym_address
  // call rax                         ff d0
  // int3                             cc
  MachineCode code;
  code.AppendBytes({0x48, 0xbf})
      .AppendImmediate64(absl::bit_cast<uint64_t>(handle))
      .AppendBytes({0x48, 0xbe})
      .AppendImmediate64(symbol_name_address)
      .AppendBytes({0x48, 0xb8})
      .AppendImmediate64(dlsym_address)
      .AppendBytes({0xff, 0xd0})
      .AppendBytes({0xcc});

  OUTCOME_TRY(return_value, ExecuteMachineCode(code_memory.get_mutable(), code));

  return absl::bit_cast<void*>(return_value);
}

[[nodiscard]] ErrorMessageOr<void> DlcloseInTracee(pid_t pid, void* handle) {
  // Figure out address of dlclose.
  OUTCOME_TRY(dlclose_address, FindFunctionAddressWithFallback(pid, kLibdlSoname, kDlcloseInLibdl,
                                                               kLibcSoname, kDlcloseInLibc));

  // Allocate small memory area in the tracee.
  auto code_memory_or_error = AllocateInTraceeAsUniqueResource(pid, 0, kCodeScratchPadSize);
  if (code_memory_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to allocate memory in tracee: %s",
                                        code_memory_or_error.error().message()));
  }
  auto code_memory = std::move(code_memory_or_error.value());

  // We want to do the following in the tracee:
  // dlclose(handle);
  // The calling convention is to put the parameter in registers rdi. Then we load the address of
  // dlclose_address into rax and do the call. Assembly in Intel syntax (destination first), machine
  // code on the right:

  // movabsq rdi, handle              48 bf handle
  // movabsq rax, dlclose_address     48 b8 dlclose_address
  // call rax                         ff d0
  // int3                             cc
  MachineCode code;
  code.AppendBytes({0x48, 0xbf})
      .AppendImmediate64(absl::bit_cast<uint64_t>(handle))
      .AppendBytes({0x48, 0xb8})
      .AppendImmediate64(dlclose_address)
      .AppendBytes({0xff, 0xd0})
      .AppendBytes({0xcc});

  OUTCOME_TRY(ExecuteMachineCode(code_memory.get_mutable(), code));

  return outcome::success();
}

}  // namespace orbit_user_space_instrumentation
