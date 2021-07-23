// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CLIENT_DATA_TRACK_DATA_H_
#define CLIENT_DATA_TRACK_DATA_H_

#include <absl/synchronization/mutex.h>

#include "ClientData/TimerChain.h"
#include "capture_data.pb.h"

namespace orbit_client_data {

class TrackData {
 public:
  [[nodiscard]] bool IsEmpty() const { return num_timers_ == 0; }
  [[nodiscard]] size_t GetNumberOfTimers() const { return num_timers_; }
  [[nodiscard]] uint64_t GetMinTime() const { return min_time_; }
  [[nodiscard]] uint64_t GetMaxTime() const { return max_time_; }

  const orbit_client_protos::TimerInfo& AddTimer(uint64_t depth,
                                                 orbit_client_protos::TimerInfo timer_info) {
    TimerChain* timer_chain = GetOrCreateTimerChain(depth);
    UpdateMinTime(timer_info.start());
    UpdateMaxTime(timer_info.end());
    ++num_timers_;

    return timer_chain->emplace_back(std::move(timer_info));
  }

  [[nodiscard]] std::vector<const TimerChain*> GetChains() const {
    std::vector<const TimerChain*> chains;
    absl::MutexLock lock(&mutex_);
    for (const auto& it : timers_) {
      chains.push_back(it.second.get());
    }

    return chains;
  }

  [[nodiscard]] const TimerChain* GetChain(uint64_t depth) const {
    absl::MutexLock lock(&mutex_);
    auto it = timers_.find(depth);
    if (it != timers_.end()) {
      return it->second.get();
    }

    return nullptr;
  }

  void UpdateMinTime(uint64_t min_time) {
    uint64_t current_min = min_time_.load();
    while ((min_time < current_min) && !min_time_.compare_exchange_weak(current_min, min_time)) {
    }
  }

  void UpdateMaxTime(uint64_t max_time) {
    uint64_t current_max = max_time_.load();
    while ((max_time > current_max) && !max_time_.compare_exchange_weak(current_max, max_time)) {
    }
  }

 private:
  [[nodiscard]] TimerChain* GetOrCreateTimerChain(uint64_t depth) {
    absl::MutexLock lock(&mutex_);
    auto it = timers_.find(depth);
    if (it != timers_.end()) {
      return it->second.get();
    }

    auto [inserted_it, inserted] = timers_.insert_or_assign(depth, std::make_unique<TimerChain>());
    CHECK(inserted);
    return inserted_it->second.get();
  }

  mutable absl::Mutex mutex_;
  std::map<uint64_t, std::unique_ptr<TimerChain>> timers_ GUARDED_BY(mutex_);
  std::atomic<size_t> num_timers_{0};
  std::atomic<uint64_t> min_time_{std::numeric_limits<uint64_t>::max()};
  std::atomic<uint64_t> max_time_{std::numeric_limits<uint64_t>::min()};
};

}  // namespace orbit_client_data

#endif  // CLIENT_DATA_TRACK_DATA_H_
