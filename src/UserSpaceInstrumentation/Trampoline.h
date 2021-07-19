// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef USER_SPACE_INSTRUMENTATION_TRAMPOLINE_H_
#define USER_SPACE_INSTRUMENTATION_TRAMPOLINE_H_

#include <absl/container/flat_hash_map.h>
#include <capstone/capstone.h>
#include <stddef.h>
#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "AddressRange.h"
#include "AllocateInTracee.h"
#include "OrbitBase/Result.h"

namespace orbit_user_space_instrumentation {

// Returns true if the ranges overlap (touching ranges do not count as overlapping). Assumes that
// the ranges are well formed (start < end).
[[nodiscard]] bool DoAddressRangesOverlap(const AddressRange& a, const AddressRange& b);

// Returns the index of the lowest range in `ranges_sorted` that is intersecting with `range`.
// `ranges_sorted` needs to contain non-overlapping ranges in ascending order (as provided by
// `GetUnavailableAddressRanges`).
[[nodiscard]] std::optional<size_t> LowestIntersectingAddressRange(
    const std::vector<AddressRange>& ranges_sorted, const AddressRange& range);

// Returns the index of the highest range in `ranges_sorted` that is intersecting with `range`.
// `ranges_sorted` needs to contain non-overlapping ranges in ascending order (as provided by
// `GetUnavailableAddressRanges`).
[[nodiscard]] std::optional<size_t> HighestIntersectingAddressRange(
    const std::vector<AddressRange>& ranges_sorted, const AddressRange& range);

// Parses the /proc/pid/maps file of a process and returns all the taken address ranges (joining
// directly neighboring ones). We also add a range [0, /proc/sys/vm/mmap_min_addr] to block the
// lowest addresses in the process space which mmap cannot use.
[[nodiscard]] ErrorMessageOr<std::vector<AddressRange>> GetUnavailableAddressRanges(pid_t pid);

// Finds an empty address range not overlapping with anything in `unavailable_ranges` of a given
// `size` suitable to allocate the trampolines close to `code_range`. "Close to" in this context
// means that the trampolines can't be more than a 32 bit offset away from the `code_range` (+-2GB)
// such that we can jump back and forth from the trampolines to the code using relative 32 bit
// addresses.
// `unavailable_ranges` needs to contain non-overlapping ranges in ascending order; the smallest
// range needs to start at zero (as provided by `GetUnavailableAddressRanges`).
[[nodiscard]] ErrorMessageOr<AddressRange> FindAddressRangeForTrampoline(
    const std::vector<AddressRange>& unavailable_ranges, const AddressRange& code_range,
    uint64_t size);

// Allocates `size` bytes in the tracee close to `code_range`. The memory segment will be placed
// such that we can jump from any position in the memory segment to any position in `code_range`
// (and vice versa) by relative jumps using 32 bit offsets.
[[nodiscard]] ErrorMessageOr<MemoryInTracee> AllocateMemoryForTrampolines(
    pid_t pid, const AddressRange& code_range, uint64_t size);

// Returns the signed 32 bit difference (a-b) between two absolute virtual 64 bit addresses or an
// error if the difference is too large.
[[nodiscard]] ErrorMessageOr<int32_t> AddressDifferenceAsInt32(uint64_t a, uint64_t b);

// Merely serves as a return value for the function below.
struct RelocatedInstruction {
  // Machine code of the relocated instruction. Might contain multiple instructions to emulate what
  // the original instruction achieved.
  std::vector<uint8_t> code;

  // Some relocated instructions contain an absolute address stored in the `code` above. That
  // address needs to be adjusted once all the relocations are done. The position of this absolute
  // address in `code` is what is stored here.
  // Example: A conditional jump to a forward position needs to know the
  // position of an instruction not yet processed.
  //
  // Original code does the following: condition cc is true -> InstructionB,
  // otherwise -> InstructionA, InstructionB
  //
  // 0x0100: jcc rip+2 (==0x0104)
  // 0x0102: InstructionA
  // 0x0104: InstructionB
  //
  // -> relocate ->
  //
  // 0x0200: j(!cc) rip+08 (== 0x0210)
  // 0x0202: jmp [rip+0] (== [0x0208])
  // 0x0208: 8 byte destination address == address of relocated InstructionB == 0x0217
  // 0x0210: InstructionA'
  // 0x0217: InstructionB'
  //
  // The conditional jump at 0x0100 is translated into the first three lines of the result. The
  // address (at 0x0208) of InstructionB' is not yet known at the point of the translation. So it
  // needs to be recorded and handled later. In this case the `position_of_absolute_address` below
  // would be 8.
  std::optional<size_t> position_of_absolute_address = std::nullopt;
};

// Relocate `instruction` from `old_address` to `new_address`.
// For many instructions the machine code can just be copied into the return value. The interesting
// cases that need handling are relative jumps and calls, loop instructions and instructions that
// use instruction pointer relative addressing (the implementation contains more detailed comments
// for all the cases).
// Returns the translated code and, optionally, a position in the code that might require an address
// translation (details in the comment above).
// Note that not all instructions can be handled (for various reasons, see the comments in the
// implemention). At least in the current implementation it might not be possible to instrument some
// functions.
[[nodiscard]] ErrorMessageOr<RelocatedInstruction> RelocateInstruction(cs_insn* instruction,
                                                                       uint64_t old_address,
                                                                       uint64_t new_address);

// Strictly speaking the max tempoline size is a compile time constant, but we prefer to compute it
// here since this captures every change to the code constructing the trampoline.
[[nodiscard]] uint64_t GetMaxTrampolineSize();

// Creates a trampoline for the function at `function_address`. The trampoline is built at
// `trampoline_address`. The trampoline will call `entry_payload_function_address` with
// `return_address` and a function id as a parameter. The function id is written into the
// trampoline by `InstrumentFunction`. This is necessary since the function id is not stable across
// multiple profiling runs.
// `function` contains the beginning of the function (kMaxFunctionPrologueBackupSize bytes or less
// if the function shorter). `capstone_handle` is a handle to the capstone disassembler library
// returned by cs_open. The function returns an error if it was not possible to instrument the
// function. For details on that see the comments at AppendRelocatedPrologueCode. If the function is
// successful it will insert an address pair into `relocation_map` for each instruction it relocated
// from the beginning of the function into the trampoline (needed for moving instruction pointers
// away from the overwritten bytes at the beginning of the function, compare
// MoveInstructionPointersOutOfOverwrittenCode below). The return value is the address of the first
// instruction not relocated into the trampoline (i.e. the address the trampoline jump back to).
[[nodiscard]] ErrorMessageOr<uint64_t> CreateTrampoline(
    pid_t pid, uint64_t function_address, const std::vector<uint8_t>& function,
    uint64_t trampoline_address, uint64_t entry_payload_function_address,
    uint64_t return_trampoline_address, csh capstone_handle,
    absl::flat_hash_map<uint64_t, uint64_t>& relocation_map);

// As above with `GetMaxTrampolineSize` this is a compile time constant, but we prefer to compute it
// here since this captures every change to the code constructing the return trampoline.
[[nodiscard]] uint64_t GetReturnTrampolineSize();

// Creates a "return trampoline" i.e. a bit of code that is used as a target for overwritten return
// addresses. It calls the function `exit_payload_function_address` and jumps to the return value of
// that function. The return trampoline is constructed at address `return_trampoline_address`.
// Unlike what is done in `CreateTrampoline` we don't need an individual trampoline for each
// function we instrument. The different functions are disambiguated by the order in which the
// function exit appears (and it is the responsibility of the payload functions to keep track of
// this). Also the return trampoline does not need to be located close (32 bit offset) to any
// specific code location; all jumps involved are to absolute 64 bit addresses.
[[nodiscard]] ErrorMessageOr<void> CreateReturnTrampoline(pid_t pid,
                                                          uint64_t exit_payload_function_address,
                                                          uint64_t return_trampoline_address);

// Instrument function at `function_address` in process `pid`. This simply overwrites the beginning
// of the fuction with a jump to `trampoline_address`. The trampoline needs to be constructed with
// `CreateTrampoline` above. The trampoline gets patched such that it hands over the current
// `function_id` to the entry payload.
[[nodiscard]] ErrorMessageOr<void> InstrumentFunction(pid_t pid, uint64_t function_address,
                                                      uint64_t function_id,
                                                      uint64_t address_of_instruction_after_jump,
                                                      uint64_t trampoline_address);

// Move every instruction pointer that was in the middle of an overwritten function prologue to
// the corresponding place in the trampoline.
void MoveInstructionPointersOutOfOverwrittenCode(
    pid_t pid, const absl::flat_hash_map<uint64_t, uint64_t>& relocation_map);

}  // namespace orbit_user_space_instrumentation

#endif  // USER_SPACE_INSTRUMENTATION_TRAMPOLINE_H_