// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <absl/base/casts.h>
#include <absl/strings/match.h>
#include <absl/strings/str_format.h>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <sys/wait.h>

#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include "AllocateInTracee.h"
#include "ExecuteMachineCode.h"
#include "MachineCode.h"
#include "OrbitBase/ExecutablePath.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/ReadFileToString.h"
#include "OrbitBase/Result.h"
#include "OrbitBase/TestUtils.h"
#include "UserSpaceInstrumentation/Attach.h"
#include "UserSpaceInstrumentation/InjectLibraryInTracee.h"

namespace orbit_user_space_instrumentation {

namespace {

using orbit_base::HasError;
using orbit_base::HasNoError;

void OpenUseAndCloseLibrary(pid_t pid) {
  // Stop the child process using our tooling.
  CHECK(!AttachAndStopProcess(pid).has_error());

  // Tracee does not have the dynamic lib loaded, obviously.
  const std::string kLibName = "libUserSpaceInstrumentationTestLib.so";
  auto maps_before = orbit_base::ReadFileToString(absl::StrFormat("/proc/%d/maps", pid));
  CHECK(maps_before.has_value());
  EXPECT_FALSE(absl::StrContains(maps_before.value(), kLibName));

  // Load dynamic lib into tracee.
  auto library_handle_or_error =
      DlopenInTracee(pid, orbit_base::GetExecutableDir() / ".." / "lib" / kLibName, RTLD_NOW);
  ASSERT_TRUE(library_handle_or_error.has_value());

  // Tracee now does have the dynamic lib loaded.
  auto maps_after_open = orbit_base::ReadFileToString(absl::StrFormat("/proc/%d/maps", pid));
  CHECK(maps_after_open.has_value());
  EXPECT_TRUE(absl::StrContains(maps_after_open.value(), kLibName));

  // Look up symbol for "TrivialFunction" in the dynamic lib.
  auto dlsym_or_error = DlsymInTracee(pid, library_handle_or_error.value(), "TrivialFunction");
  ASSERT_TRUE(dlsym_or_error.has_value());
  const uint64_t function_address = absl::bit_cast<uint64_t>(dlsym_or_error.value());

  {
    // Write machine code to call "TrivialFunction" from the dynamic lib.
    constexpr uint64_t kScratchPadSize = 1024;
    auto memory_or_error = AllocateInTraceeAsUniqueResource(pid, 0, kScratchPadSize);
    ASSERT_TRUE(memory_or_error.has_value());
    // Move function's address to rax, do the call, and hit a breakpoint:
    // movabs rax, function_address     48 b8 function_address
    // call rax                         ff d0
    // int3                             cc
    MachineCode code;
    code.AppendBytes({0x48, 0xb8})
        .AppendImmediate64(function_address)
        .AppendBytes({0xff, 0xd0})
        .AppendBytes({0xcc});

    auto result_or_error = ExecuteMachineCode(memory_or_error.value().get_mutable(), code);
    ASSERT_THAT(result_or_error, HasNoError());
    EXPECT_EQ(42, result_or_error.value());
  }

  // Close the library.
  auto result_dlclose = DlcloseInTracee(pid, library_handle_or_error.value());
  ASSERT_THAT(result_dlclose, HasNoError());

  // Now, again, the lib is absent from the tracee.
  auto maps_after_close = orbit_base::ReadFileToString(absl::StrFormat("/proc/%d/maps", pid));
  CHECK(maps_after_close.has_value());
  EXPECT_FALSE(absl::StrContains(maps_after_close.value(), kLibName));

  CHECK(!DetachAndContinueProcess(pid).has_error());
}

}  // namespace

TEST(InjectLibraryInTraceeTest, OpenUseAndCloseLibraryInUserCode) {
  pid_t pid = fork();
  CHECK(pid != -1);
  if (pid == 0) {
    while (true) {
    }
  }

  OpenUseAndCloseLibrary(pid);

  // End child process.
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

TEST(InjectLibraryInTraceeTest, OpenUseAndCloseLibraryInSyscall) {
  pid_t pid = fork();
  CHECK(pid != -1);
  if (pid == 0) {
    while (true) {
      // Child will be stuck in syscall sys_clock_nanosleep.
      std::this_thread::sleep_for(std::chrono::hours(1000000000));
    }
  }

  OpenUseAndCloseLibrary(pid);

  // End child process.
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

TEST(InjectLibraryInTraceeTest, NonExistingLibrary) {
  pid_t pid = fork();
  CHECK(pid != -1);
  if (pid == 0) {
    while (true) {
      // Child will be stuck in syscall sys_clock_nanosleep.
      std::this_thread::sleep_for(std::chrono::hours(1000000000));
    }
  }

  // Stop the child process using our tooling.
  CHECK(!AttachAndStopProcess(pid).has_error());

  // Try to load non existing dynamic lib into tracee.
  const std::string kNonExistingLibName = "libNotFound.so";
  auto library_handle_or_error = DlopenInTracee(pid, kNonExistingLibName, RTLD_NOW);
  ASSERT_THAT(library_handle_or_error, HasError("Library does not exist at"));

  // Continue child process.
  CHECK(!DetachAndContinueProcess(pid).has_error());

  // End child process.
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

}  // namespace orbit_user_space_instrumentation
