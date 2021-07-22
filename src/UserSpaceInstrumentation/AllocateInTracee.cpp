// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "AllocateInTracee.h"

#include <absl/base/casts.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_split.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include <cstdint>
#include <string>
#include <vector>

#include "AccessTraceesMemory.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/SafeStrerror.h"
#include "RegisterState.h"

namespace orbit_user_space_instrumentation {

namespace {

// Execute a single syscall instruction in tracee `pid`. `syscall` identifies the syscall as in this
// list: https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl
// The parameters and the ordering are the same as in the C wrapper:
// https://man7.org/linux/man-pages/dir_section_2.html
// Optionally one can specify `exclude_address`. This prevents the method from using an address
// range containing `exclude_address` as a working area. This is required for the munmap syscall
// which might otherwise choose the mapping it is removing as a working area.
[[nodiscard]] ErrorMessageOr<uint64_t> SyscallInTracee(pid_t pid, uint64_t syscall, uint64_t arg_0,
                                                       uint64_t arg_1, uint64_t arg_2,
                                                       uint64_t arg_3, uint64_t arg_4,
                                                       uint64_t arg_5, uint64_t exclude_address) {
  RegisterState original_registers;
  auto register_backup_result = original_registers.BackupRegisters(pid);
  if (register_backup_result.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to backup original register state: \"%s\"",
                                        register_backup_result.error().message()));
  }
  if (original_registers.GetBitness() != RegisterState::Bitness::k64Bit) {
    return ErrorMessage(
        "Tried to invoke syscall in 32 bit process. This is currently not supported.");
  }

  // Get an executable memory region.
  auto memory_region_or_error = GetFirstExecutableMemoryRegion(pid, exclude_address);
  if (memory_region_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to find executable memory region: \"%s\"",
                                        memory_region_or_error.error().message()));
  }
  const uint64_t start_address = memory_region_or_error.value().start;

  // Backup first 8 bytes.
  auto backup_or_error = ReadTraceesMemory(pid, start_address, 8);
  if (backup_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to read from tracee's memory: \"%s\"",
                                        backup_or_error.error().message()));
  }

  // Write `syscall` into memory. Machine code is `0x0f05`.
  auto write_code_result = WriteTraceesMemory(pid, start_address, std::vector<uint8_t>{0x0f, 0x05});
  if (write_code_result.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to write to tracee's memory: \"%s\"",
                                        write_code_result.error().message()));
  }

  // Move instruction pointer to the `syscall` and fill registers with parameters.
  RegisterState registers_for_syscall = original_registers;
  registers_for_syscall.GetGeneralPurposeRegisters()->x86_64.rip = start_address;
  registers_for_syscall.GetGeneralPurposeRegisters()->x86_64.rax = syscall;
  // Register list for arguments can be found e.g. in the glibc wrapper:
  // https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/x86_64/syscall.S#L30
  registers_for_syscall.GetGeneralPurposeRegisters()->x86_64.rdi = arg_0;
  registers_for_syscall.GetGeneralPurposeRegisters()->x86_64.rsi = arg_1;
  registers_for_syscall.GetGeneralPurposeRegisters()->x86_64.rdx = arg_2;
  registers_for_syscall.GetGeneralPurposeRegisters()->x86_64.r10 = arg_3;
  registers_for_syscall.GetGeneralPurposeRegisters()->x86_64.r8 = arg_4;
  registers_for_syscall.GetGeneralPurposeRegisters()->x86_64.r9 = arg_5;
  auto restore_registers_result = registers_for_syscall.RestoreRegisters();
  if (restore_registers_result.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to set registers with syscall parameters: \"%s\"",
                                        restore_registers_result.error().message()));
  }

  // Single step to execute the syscall.
  if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0) != 0) {
    return ErrorMessage("Failed to execute syscall with PTRACE_SINGLESTEP.");
  }
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  if (waited != pid || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
    return ErrorMessage("Failed to wait for PTRACE_SINGLESTEP to execute.");
  }

  // Return value of syscalls is in rax.
  RegisterState return_value;
  auto return_value_result = return_value.BackupRegisters(pid);
  if (return_value_result.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to get registers with mmap result: \"%s\"",
                                        return_value_result.error().message()));
  }
  const uint64_t result = return_value.GetGeneralPurposeRegisters()->x86_64.rax;
  // Syscalls return -4095, ..., -1 on failure. And these are actually (-1 * errno)
  const int64_t result_as_int = absl::bit_cast<int64_t>(result);
  if (result_as_int > -4096 && result_as_int < 0) {
    return ErrorMessage(absl::StrFormat("Syscall failed. Return value: %s (%d)",
                                        SafeStrerror(-result_as_int), result_as_int));
  }

  // Clean up memory and registers.
  auto restore_memory_result = WriteTraceesMemory(pid, start_address, backup_or_error.value());
  if (restore_memory_result.has_error()) {
    FATAL("Unable to restore memory state of tracee: \"%s\"",
          restore_memory_result.error().message());
  }
  restore_registers_result = original_registers.RestoreRegisters();
  if (restore_registers_result.has_error()) {
    FATAL("Unable to restore register state of tracee: \"%s\"",
          restore_registers_result.error().message());
  }

  return result;
}

}  // namespace

ErrorMessageOr<void> MemoryInTracee::Allocate(pid_t pid, uint64_t address, uint64_t size) {
  // Syscall will be equivalent to:
  // `mmap(address, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)`
  constexpr uint64_t kSyscallNumberMmap = 9;
  auto result_or_error = SyscallInTracee(pid, kSyscallNumberMmap, address, size, PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, static_cast<uint64_t>(-1), 0,
                                         /*exclude_address=*/0);
  if (result_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat(
        "Failed to execute mmap syscall with parameters address=%#x size=%u: \"%s\"", address, size,
        result_or_error.error().message()));
  }
  pid_ = pid;
  address_ = result_or_error.value();
  size_ = size;
  state_ = MemoryInTracee::State::kWritable;

  if (address != 0 && address_ != address) {
    auto free_memory_result = Free();
    FAIL_IF(free_memory_result.has_error(), "Unable to free proviously allocated memory: \"%s\"",
            free_memory_result.error().message());
    return ErrorMessage(
        absl::StrFormat("AllocateInTracee wanted to allocate memory at %#x but got memory at a "
                        "different adress: %#x. The memory has been freed again.",
                        address, address_));
  }
  return outcome::success();
}

ErrorMessageOr<void> MemoryInTracee::Free() {
  // Syscall will be equivalent to:
  // `munmap(address, size)`
  constexpr uint64_t kSyscallNumberMunmap = 11;
  auto result_or_error =
      SyscallInTracee(pid_, kSyscallNumberMunmap, address_, size_, 0, 0, 0, 0, address_);
  if (result_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to execute munmap syscall: \"%s\"",
                                        result_or_error.error().message()));
  }
  pid_ = -1;
  address_ = 0;
  size_ = 0;
  state_ = State::kInvalid;
  return outcome::success();
}

ErrorMessageOr<void> MemoryInTracee::MakeMemoryExecutable() {
  if (state_ == State::kExecutable) {
    return outcome::success();
  }

  constexpr uint64_t kSyscallNumberMprotect = 10;
  auto result_or_error = SyscallInTracee(pid_, kSyscallNumberMprotect, address_, size_,
                                         PROT_READ | PROT_EXEC, 0, 0, 0, 0);
  if (result_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat(
        "Failed to execute mprotect syscall with parameters address=%#x size=%u: \"%s\"", address_,
        size_, result_or_error.error().message()));
  }

  state_ = State::kExecutable;
  return outcome::success();
}

ErrorMessageOr<void> MemoryInTracee::MakeMemoryWritable() {
  if (state_ == State::kWritable) {
    return outcome::success();
  }

  constexpr uint64_t kSyscallNumberMprotect = 10;
  auto result_or_error =
      SyscallInTracee(pid_, kSyscallNumberMprotect, address_, size_, PROT_WRITE, 0, 0, 0, 0);
  if (result_or_error.has_error()) {
    return ErrorMessage(absl::StrFormat(
        "Failed to execute mprotect syscall with parameters address=%#x size=%u: \"%s\"", address_,
        size_, result_or_error.error().message()));
  }

  state_ = State::kWritable;
  return outcome::success();
}

ErrorMessageOr<orbit_base::unique_resource<MemoryInTracee, std::function<void(MemoryInTracee&)>>>
AllocateInTraceeAsUniqueResource(pid_t pid, uint64_t address, uint64_t size) {
  MemoryInTracee memory;
  OUTCOME_TRY(memory.Allocate(pid, address, size));
  std::function<void(MemoryInTracee&)> deleter = [](MemoryInTracee& memory) {
    auto result = memory.Free();
    if (result.has_error()) {
      ERROR("Unable to free previously allocated memory in tracee: \"%s\"",
            result.error().message());
    }
  };
  return orbit_base::unique_resource(std::move(memory), deleter);
}

}  // namespace orbit_user_space_instrumentation