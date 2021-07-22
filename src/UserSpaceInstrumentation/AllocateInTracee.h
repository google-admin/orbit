// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef USER_SPACE_INSTRUMENTATION_ALLOCATE_IN_TRACEE_H_
#define USER_SPACE_INSTRUMENTATION_ALLOCATE_IN_TRACEE_H_

#include <sys/types.h>

#include <cstdint>
#include <functional>

#include "OrbitBase/Result.h"
#include "OrbitBase/UniqueResource.h"

namespace orbit_user_space_instrumentation {

// Represents a chunk of memory in the tracee. This class is movable but not copyable so it is a
// unique handle to that memory in the tracee.
// `MemoryInTracee` gets default constructed in `kInvalid` state. The first thing that needs to
// happen is to actually allocate memory in the trace using `Allocate`. The memory gets allocated in
// writable state. In case one wants to execute code in the segment it needs to be made executable
// with `MakeMemoryExecutable` later.
// `Free` deallocates the memory. Note that for each of the non-const member functions we need to
// execute code in the tracee. So we need to be attached; the tracee needs to be stopped.
class MemoryInTracee {
 public:
  MemoryInTracee() = default;
  MemoryInTracee(const MemoryInTracee&) = delete;
  MemoryInTracee(MemoryInTracee&&) = default;
  MemoryInTracee& operator=(const MemoryInTracee&) = delete;
  MemoryInTracee& operator=(MemoryInTracee&&) = default;

  // Allocate `size` bytes of memory in the tracee's address space using mmap. The memory will have
  // write permissions. The memory allocated will start at `address`. `address` needs to be aligned
  // to page boundaries. If the memory mapping can not be placed at `address` an error is returned.
  // If `address` is zero the placement of memory will be arbitray (compare the documenation of
  // mmap: https://man7.org/linux/man-pages/man2/mmap.2.html). Assumes we are already attached to
  // the tracee `pid` using `AttachAndStopProcess`.
  [[nodiscard]] ErrorMessageOr<void> Allocate(pid_t pid, uint64_t address, uint64_t size);

  // Free address range previously allocated with AllocateInTracee using munmap.
  // Assumes we are already attached to the tracee using `AttachAndStopProcess`.
  [[nodiscard]] ErrorMessageOr<void> Free();

  [[nodiscard]] pid_t GetPid() const { return pid_; }
  [[nodiscard]] uint64_t GetAddress() const { return address_; }
  [[nodiscard]] uint64_t GetSize() const { return size_; }
  // Returns the protection state of the memory. The memory get allocated as writeable and can be
  // made executable (and writable again)  with the methods below.
  enum class State { kInvalid, kWritable, kExecutable };
  [[nodiscard]] State GetState() const { return state_; }

  // Sets the read and execute permission on the memory. Removes the write permission. Assumes we
  // are already attached to the tracee using `AttachAndStopProcess`.

  [[nodiscard]] ErrorMessageOr<void> MakeMemoryExecutable();
  // Set the write permission on the memory. Removes the read and execute permmissions. Assumes we
  // are already attached to the tracee using `AttachAndStopProcess`.
  [[nodiscard]] ErrorMessageOr<void> MakeMemoryWritable();

 private:
  pid_t pid_ = -1;
  uint64_t address_ = 0;
  uint64_t size_ = 0;
  State state_ = State::kInvalid;
};

// Produce a `MemoryInTracee` object, call `Allocate` on that object and wrap it in a
// unique_resource that handles deallocating the memory. Note that we still need to be attached (or
// attached again) to the tracee when the unique_resource goes out of scope.
[[nodiscard]] ErrorMessageOr<
    orbit_base::unique_resource<MemoryInTracee, std::function<void(MemoryInTracee&)>>>
AllocateInTraceeAsUniqueResource(pid_t pid, uint64_t address, uint64_t size);
}  // namespace orbit_user_space_instrumentation

#endif  // USER_SPACE_INSTRUMENTATION_ALLOCATE_IN_TRACEE_H_