// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_CORE_TIMER_H_
#define ORBIT_CORE_TIMER_H_

#include <string>

#include "OrbitBase/Profiling.h"

class Timer {
 public:
  void Start() { start_ = MonotonicTimestampNs(); }

  void Stop() { end_ = MonotonicTimestampNs(); }

  void Reset() {
    Stop();
    Start();
  }

  [[nodiscard]] double ElapsedMicros() const { return TicksToMicroseconds(start_, end_); }

  [[nodiscard]] double ElapsedMillis() const { return ElapsedMicros() * 0.001; }
  [[nodiscard]] double ElapsedSeconds() const { return ElapsedMicros() * 0.000001; }

  double QueryMillis() {
    Stop();
    return ElapsedMillis();
  }

  double QuerySeconds() {
    Stop();
    return ElapsedSeconds();
  }

 private:
  uint64_t start_ = 0;
  uint64_t end_ = 0;
};

#endif  // ORBIT_CORE_TIMER_H_
