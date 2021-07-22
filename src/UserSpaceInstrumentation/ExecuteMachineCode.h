// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef USER_SPACE_INSTRUMENTATION_EXECUTE_MACHINE_CODE_H_
#define USER_SPACE_INSTRUMENTATION_EXECUTE_MACHINE_CODE_H_

#include <sys/types.h>

#include <cstdint>

#include "AllocateInTracee.h"
#include "MachineCode.h"
#include "OrbitBase/Result.h"

namespace orbit_user_space_instrumentation {

// Copies `code` to `code_memory` in the tracee and executes it. The memory at `code_memory` needs
// to be allocated using AllocateInTracee. The code segment has to end with an `int3`. Takes care of
// backup and restore of register state in the tracee.
// The return value is the content of rax after the execution finished.
[[nodiscard]] ErrorMessageOr<uint64_t> ExecuteMachineCode(MemoryInTracee& code_memory,
                                                          const MachineCode& code);

}  // namespace orbit_user_space_instrumentation

#endif  // USER_SPACE_INSTRUMENTATION_EXECUTE_MACHINE_CODE_H_