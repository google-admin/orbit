// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <random>
#include <vector>

#include "AccessTraceesMemory.h"
#include "OrbitBase/Logging.h"
#include "UserSpaceInstrumentation/Attach.h"
#include "UserSpaceInstrumentation/RegisterState.h"

namespace orbit_user_space_instrumentation {

TEST(AccessTraceesMemoryTest, ReadWriteRestore) {
  pid_t pid = fork();
  CHECK(pid != -1);
  if (pid == 0) {
    // Child just runs an endless loop.
    while (true) {
    }
  }

  // Stop the child process using our tooling.
  CHECK(AttachAndStopProcess(pid).has_value());

  uint64_t address_start = 0;
  uint64_t address_end = 0;
  auto result_memory_region = GetFirstExecutableMemoryRegion(pid, &address_start, &address_end);
  CHECK(result_memory_region.has_value());

  auto result_backup = ReadTraceesMemory(pid, address_start, address_end - address_start);
  CHECK(result_backup.has_value());

  std::vector<uint8_t> new_data(result_backup.value().size());
  std::mt19937 engine{std::random_device()()};
  std::uniform_int_distribution<uint8_t> distribution{0x00, 0xff};
  std::generate(std::begin(new_data), std::end(new_data),
                [&distribution, &engine]() { return distribution(engine); });

  CHECK(WriteTraceesMemory(pid, address_start, new_data).has_value());

  auto result_read_back = ReadTraceesMemory(pid, address_start, address_end - address_start);
  CHECK(result_read_back.has_value());

  EXPECT_EQ(new_data, result_read_back.value());

  CHECK(WriteTraceesMemory(pid, address_start, result_backup.value()).has_value());

  // Detach and end child.
  CHECK(DetachAndContinueProcess(pid).has_value());
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

}  // namespace orbit_user_space_instrumentation