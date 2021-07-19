// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <absl/strings/numbers.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_split.h>
#include <gtest/gtest.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <csignal>
#include <filesystem>
#include <string>
#include <vector>

#include "AllocateInTracee.h"
#include "OrbitBase/ExecutablePath.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/ReadFileToString.h"
#include "OrbitBase/TestUtils.h"
#include "UserSpaceInstrumentation/Attach.h"

namespace orbit_user_space_instrumentation {

namespace {

using orbit_base::HasError;
using orbit_base::HasNoError;
using orbit_base::ReadFileToString;

enum class ProtState { kWrite, kReadExec };

// Returns true if the target process has writeable (or executable; depending on `state`) memory
// segment at the address of `memory`.
[[nodiscard]] bool ProcessHasMapAtAddress(const MemoryInTracee& memory, ProtState state) {
  auto maps_or_error = ReadFileToString(absl::StrFormat("/proc/%d/maps", memory.GetPid()));
  CHECK(maps_or_error.has_value());
  std::vector<std::string> lines = absl::StrSplit(maps_or_error.value(), '\n', absl::SkipEmpty());
  for (const auto& line : lines) {
    std::vector<std::string> tokens = absl::StrSplit(line, ' ', absl::SkipEmpty());
    if (state == ProtState::kWrite) {
      if (tokens.size() < 2 || tokens[1].size() < 2 || tokens[1][1] != 'w') {
        continue;
      }
    } else if (state == ProtState::kReadExec) {
      if (tokens.size() < 2 || tokens[1].size() < 3 || tokens[1][0] != 'r' || tokens[1][2] != 'x') {
        continue;
      }
    }
    std::vector<std::string> addresses = absl::StrSplit(tokens[0], '-');
    if (addresses.size() != 2) {
      continue;
    }
    if (std::stoull(addresses[0], nullptr, 16) == memory.GetAddress()) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(AllocateInTraceeTest, AllocateAndFree) {
  pid_t pid = fork();
  CHECK(pid != -1);
  if (pid == 0) {
    // Child just runs an endless loop.
    while (true) {
    }
  }

  // Stop the process using our tooling.
  CHECK(!AttachAndStopProcess(pid).has_error());

  // Allocation fails for invalid process.
  constexpr uint64_t kMemorySize = 1024 * 1024;
  MemoryInTracee my_memory;
  EXPECT_THAT(my_memory.Allocate(-1, 0, kMemorySize), HasError("No such process"));

  // Allocation fails for non page aligned address.
  EXPECT_THAT(my_memory.Allocate(pid, 1, kMemorySize),
              HasError("but got memory at a different adress"));

  // Allocation fails for ridiculous size.
  EXPECT_THAT(my_memory.Allocate(pid, 0, 1ull << 63),
              HasError("Syscall failed. Return value: Cannot allocate memory"));

  // Allocate a megabyte in the tracee.
  ASSERT_THAT(my_memory.Allocate(pid, 0, kMemorySize), HasNoError());
  EXPECT_TRUE(ProcessHasMapAtAddress(my_memory, ProtState::kWrite));

  // Free the memory.
  ASSERT_THAT(my_memory.Free(), HasNoError());

  // Allocate a megabyte at a low memory position.
  auto mmap_min_addr_or_error = ReadFileToString("/proc/sys/vm/mmap_min_addr");
  CHECK(mmap_min_addr_or_error.has_value());
  uint64_t mmap_min_addr = 0;
  CHECK(absl::SimpleAtoi(mmap_min_addr_or_error.value(), &mmap_min_addr));
  ASSERT_THAT(my_memory.Allocate(pid, mmap_min_addr, kMemorySize), HasNoError());
  EXPECT_TRUE(ProcessHasMapAtAddress(my_memory, ProtState::kWrite));

  // Make memory executable.
  ASSERT_THAT(my_memory.MakeMemoryExecutable(), HasNoError());
  EXPECT_TRUE(ProcessHasMapAtAddress(my_memory, ProtState::kReadExec));

  // Make memory writable again.
  ASSERT_THAT(my_memory.MakeMemoryWritable(), HasNoError());
  EXPECT_TRUE(ProcessHasMapAtAddress(my_memory, ProtState::kWrite));

  // Free the memory.
  ASSERT_THAT(my_memory.Free(), HasNoError());

  // Allocate as unique resource.
  {
    auto unique_resource_or_error = AllocateInTraceeAsUniqueResource(pid, 0, kMemorySize);
    ASSERT_THAT(unique_resource_or_error, HasNoError());
    EXPECT_TRUE(
        ProcessHasMapAtAddress(unique_resource_or_error.value().get_mutable(), ProtState::kWrite));
  }

  // Detach and end child.
  CHECK(!DetachAndContinueProcess(pid).has_error());
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
}

}  // namespace orbit_user_space_instrumentation