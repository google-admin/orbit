// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ExecuteMachineCode.h"

#include <sys/ptrace.h>
#include <sys/wait.h>

#include <vector>

#include "AccessTraceesMemory.h"
#include "OrbitBase/Logging.h"
#include "RegisterState.h"

namespace orbit_user_space_instrumentation {

namespace {

// In certain error conditions the tracee is damaged and we don't try to recover from that. We just
// abort with a fatal log message. None of these errors are expected to occur in operation
// obvioulsy. That's what the *OrDie methods below are for.
void RestoreRegistersOrDie(RegisterState& register_state) {
  auto result = register_state.RestoreRegisters();
  FAIL_IF(result.has_error(), "Unable to restore register state in tracee: \"%s\"",
          result.error().message());
}

uint64_t GetReturnValueOrDie(pid_t pid) {
  RegisterState return_value_registers;
  auto result_return_value = return_value_registers.BackupRegisters(pid);
  FAIL_IF(result_return_value.has_error(), "Unable to read registers after function called :\"%s\"",
          result_return_value.error().message());
  return return_value_registers.GetGeneralPurposeRegisters()->x86_64.rax;
}

}  // namespace

ErrorMessageOr<uint64_t> ExecuteMachineCode(MemoryInTracee& code_memory, const MachineCode& code) {
  const pid_t pid = code_memory.GetPid();

  OUTCOME_TRY(WriteTraceesMemory(pid, code_memory.GetAddress(), code.GetResultAsVector()));

  OUTCOME_TRY(code_memory.MakeMemoryExecutable());

  // Backup registers.
  RegisterState original_registers;
  OUTCOME_TRY(original_registers.BackupRegisters(pid));

  RegisterState registers_for_execution = original_registers;
  registers_for_execution.GetGeneralPurposeRegisters()->x86_64.rip = code_memory.GetAddress();
  // The calling convention for x64 assumes the 128 bytes below rsp to be usable as a scratch pad
  // for the current function. This area is called the 'red zone'. The function we interrupted might
  // have stored temporary data in the red zone and the function we are about to execute might do
  // the same. To keep them separated we decrement rsp by 128 bytes.
  // The calling convention also asks for rsp being to be aligned to 16 bytes so we additionally
  // round down to the previous multiple of 16.
  const uint64_t old_rsp = original_registers.GetGeneralPurposeRegisters()->x86_64.rsp;
  constexpr int kSizeRedZone = 128;
  constexpr int kStackAlignment = 16;
  const uint64_t aligned_rsp_below_red_zone =
      ((old_rsp - kSizeRedZone) / kStackAlignment) * kStackAlignment;
  registers_for_execution.GetGeneralPurposeRegisters()->x86_64.rsp = aligned_rsp_below_red_zone;
  // In case we stopped the process in the middle of a syscall orig_rax holds the number of that
  // syscall. The kernel uses that to trigger the restart of the interrupted syscall. By setting
  // orig_rax to -1 we bypass this logic for the PTRACE_CONT below.
  // The syscall will be restarted when we restore the original registers and detach to continue the
  // normal operation.
  registers_for_execution.GetGeneralPurposeRegisters()->x86_64.orig_rax = -1;
  OUTCOME_TRY(registers_for_execution.RestoreRegisters());
  if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) {
    FATAL("Unable to continue tracee with PTRACE_CONT.");
  }
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  if (waited != pid || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
    FATAL(
        "Failed to wait for sigtrap after PTRACE_CONT. Expected pid: %d Pid returned from waitpid: "
        "%d status: %u, WIFSTOPPED: %u, WSTOPSIG: %u",
        pid, waited, status, WIFSTOPPED(status), WSTOPSIG(status));
  }

  const uint64_t return_value = GetReturnValueOrDie(pid);

  // Clean up registers.
  RestoreRegistersOrDie(original_registers);
  return return_value;
}

}  // namespace orbit_user_space_instrumentation