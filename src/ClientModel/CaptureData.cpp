// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ClientModel/CaptureData.h"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <memory>
#include <outcome.hpp>
#include <vector>

#include "ClientData/FunctionUtils.h"
#include "ClientData/ModuleData.h"

using orbit_client_data::CallstackData;
using orbit_client_data::ModuleData;
using orbit_client_data::TracepointData;

using orbit_client_protos::FunctionInfo;
using orbit_client_protos::FunctionStats;
using orbit_client_protos::LinuxAddressInfo;
using orbit_client_protos::ThreadStateSliceInfo;

using orbit_grpc_protos::CaptureStarted;
using orbit_grpc_protos::InstrumentedFunction;
using orbit_grpc_protos::ProcessInfo;

namespace orbit_client_model {

CaptureData::CaptureData(orbit_client_data::ModuleManager* module_manager,
                         const CaptureStarted& capture_started,
                         std::optional<std::filesystem::path> file_path,
                         absl::flat_hash_set<uint64_t> frame_track_function_ids)
    : module_manager_{module_manager},
      callstack_data_(std::make_unique<CallstackData>()),
      selection_callstack_data_(std::make_unique<CallstackData>()),
      tracepoint_data_(std::make_unique<TracepointData>()),
      frame_track_function_ids_{std::move(frame_track_function_ids)},
      file_path_{std::move(file_path)} {
  ProcessInfo process_info;
  process_info.set_pid(capture_started.process_id());
  std::filesystem::path executable_path{capture_started.executable_path()};
  process_info.set_full_path(executable_path.string());
  process_info.set_name(executable_path.filename().string());
  process_info.set_is_64_bit(true);
  process_.SetProcessInfo(process_info);

  absl::flat_hash_map<uint64_t, FunctionInfo> instrumented_functions;

  for (const auto& instrumented_function :
       capture_started.capture_options().instrumented_functions()) {
    instrumented_functions_.insert_or_assign(instrumented_function.function_id(),
                                             instrumented_function);
  }
}

void CaptureData::ForEachThreadStateSliceIntersectingTimeRange(
    int32_t thread_id, uint64_t min_timestamp, uint64_t max_timestamp,
    const std::function<void(const ThreadStateSliceInfo&)>& action) const {
  absl::MutexLock lock{thread_state_slices_mutex_.get()};
  auto tid_thread_state_slices_it = thread_state_slices_.find(thread_id);
  if (tid_thread_state_slices_it == thread_state_slices_.end()) {
    return;
  }

  const std::vector<ThreadStateSliceInfo>& tid_thread_state_slices =
      tid_thread_state_slices_it->second;
  auto slice_it = std::lower_bound(tid_thread_state_slices.begin(), tid_thread_state_slices.end(),
                                   min_timestamp,
                                   [](const ThreadStateSliceInfo& slice, uint64_t min_timestamp) {
                                     return slice.end_timestamp_ns() < min_timestamp;
                                   });
  while (slice_it != tid_thread_state_slices.end() &&
         slice_it->begin_timestamp_ns() < max_timestamp) {
    action(*slice_it);
    ++slice_it;
  }
}

const FunctionStats& CaptureData::GetFunctionStatsOrDefault(
    uint64_t instrumented_function_id) const {
  static const FunctionStats kDefaultFunctionStats;
  auto function_stats_it = functions_stats_.find(instrumented_function_id);
  if (function_stats_it == functions_stats_.end()) {
    return kDefaultFunctionStats;
  }
  return function_stats_it->second;
}

void CaptureData::UpdateFunctionStats(uint64_t instrumented_function_id, uint64_t elapsed_nanos) {
  FunctionStats& stats = functions_stats_[instrumented_function_id];
  stats.set_count(stats.count() + 1);
  stats.set_total_time_ns(stats.total_time_ns() + elapsed_nanos);
  uint64_t old_avg = stats.average_time_ns();
  stats.set_average_time_ns(stats.total_time_ns() / stats.count());
  // variance(N) = ( (N-1)*variance(N-1) + (x-avg(N))*(x-avg(N-1)) ) / N
  stats.set_variance_ns(((stats.count() - 1) * stats.variance_ns() +
                         (elapsed_nanos - stats.average_time_ns()) * (elapsed_nanos - old_avg)) /
                        static_cast<double>(stats.count()));
  // std_dev = sqrt(variance)
  stats.set_std_dev_ns(static_cast<uint64_t>(sqrt(stats.variance_ns())));

  if (elapsed_nanos > stats.max_ns()) {
    stats.set_max_ns(elapsed_nanos);
  }

  if (stats.min_ns() == 0 || elapsed_nanos < stats.min_ns()) {
    stats.set_min_ns(elapsed_nanos);
  }
}

void CaptureData::OnCaptureComplete(std::vector<orbit_client_data::TimerChain*> chains) {
  // Recalculate standard deviation as the running calculation may have introduced error.
  for (auto& pair : functions_stats_) {
    FunctionStats& stats = pair.second;
    stats.set_variance_ns(0);
  }

  for (auto& chain : chains) {
    if (!chain) continue;
    for (const auto& block : *chain) {
      for (uint64_t i = 0; i < block.size(); i++) {
        const orbit_client_protos::TimerInfo& timer_info = block[i];
        auto& stats = functions_stats_[timer_info.function_id()];
        if (stats.count() > 0) {
          uint64_t elapsed_nanos = timer_info.end() - timer_info.start();
          uint64_t diff = elapsed_nanos - stats.average_time_ns();
          stats.set_variance_ns(stats.variance_ns() + diff * diff);
        }
      }
    }
  }

  for (auto& pair : functions_stats_) {
    FunctionStats& stats = pair.second;
    if (stats.count() > 0) {
      stats.set_variance_ns(stats.variance_ns() / static_cast<double>(stats.count()));
      stats.set_std_dev_ns(static_cast<uint64_t>(sqrt(stats.variance_ns())));
    }
  }
}

const InstrumentedFunction* CaptureData::GetInstrumentedFunctionById(uint64_t function_id) const {
  auto instrumented_functions_it = instrumented_functions_.find(function_id);
  if (instrumented_functions_it == instrumented_functions_.end()) {
    return nullptr;
  }
  return &instrumented_functions_it->second;
}

std::optional<uint64_t> CaptureData::FindInstrumentedFunctionIdSlow(
    const orbit_client_protos::FunctionInfo& function) const {
  const ModuleData* module = module_manager_->GetModuleByPathAndBuildId(function.module_path(),
                                                                        function.module_build_id());
  for (const auto& it : instrumented_functions_) {
    const auto& target_function = it.second;
    if (target_function.file_path() == function.module_path() &&
        target_function.file_offset() ==
            orbit_client_data::function_utils::Offset(function, *module)) {
      return it.first;
    }
  }

  return std::nullopt;
}

const LinuxAddressInfo* CaptureData::GetAddressInfo(uint64_t absolute_address) const {
  auto address_info_it = address_infos_.find(absolute_address);
  if (address_info_it == address_infos_.end()) {
    return nullptr;
  }
  return &address_info_it->second;
}

void CaptureData::InsertAddressInfo(LinuxAddressInfo address_info) {
  const uint64_t absolute_address = address_info.absolute_address();
  const uint64_t absolute_function_address = absolute_address - address_info.offset_in_function();
  // Ensure we know the symbols also for the resolved function address;
  if (!address_infos_.contains(absolute_function_address)) {
    LinuxAddressInfo function_info;
    function_info.CopyFrom(address_info);
    function_info.set_absolute_address(absolute_function_address);
    function_info.set_offset_in_function(0);
    address_infos_.emplace(absolute_function_address, function_info);
  }
  address_infos_.emplace(absolute_address, std::move(address_info));
}

const std::string CaptureData::kUnknownFunctionOrModuleName{"???"};

const std::string& CaptureData::GetFunctionNameByAddress(uint64_t absolute_address) const {
  const FunctionInfo* function = FindFunctionByAddress(absolute_address, false);
  if (function != nullptr) {
    return orbit_client_data::function_utils::GetDisplayName(*function);
  }
  const auto address_info_it = address_infos_.find(absolute_address);
  if (address_info_it == address_infos_.end()) {
    return kUnknownFunctionOrModuleName;
  }
  const LinuxAddressInfo& address_info = address_info_it->second;
  const std::string& function_name = address_info.function_name();
  if (function_name.empty()) {
    return kUnknownFunctionOrModuleName;
  }
  return function_name;
}

// Find the start address of the function this address falls inside. Use the function returned by
// FindFunctionByAddress, and when this fails (e.g., the module containing the function has not
// been loaded) use (for now) the LinuxAddressInfo that is collected for every address in a
// callstack.
std::optional<uint64_t> CaptureData::FindFunctionAbsoluteAddressByInstructionAbsoluteAddress(
    uint64_t absolute_address) const {
  auto result =
      FindFunctionAbsoluteAddressByInstructionAbsoluteAddressUsingModulesInMemory(absolute_address);
  if (result.has_value()) {
    return result;
  }
  return FindFunctionAbsoluteAddressByInstructionAbsoluteAddressUsingAddressInfo(absolute_address);
}

std::optional<uint64_t>
CaptureData::FindFunctionAbsoluteAddressByInstructionAbsoluteAddressUsingAddressInfo(
    uint64_t absolute_address) const {
  const LinuxAddressInfo* address_info = GetAddressInfo(absolute_address);
  if (address_info == nullptr) return std::nullopt;

  return absolute_address - address_info->offset_in_function();
}

std::optional<uint64_t>
CaptureData::FindFunctionAbsoluteAddressByInstructionAbsoluteAddressUsingModulesInMemory(
    uint64_t absolute_address) const {
  const auto module_or_error = process_.FindModuleByAddress(absolute_address);
  if (module_or_error.has_error()) return std::nullopt;
  const std::string& module_path = module_or_error.value().file_path();
  const std::string& module_build_id = module_or_error.value().build_id();
  const uint64_t module_base_address = module_or_error.value().start();

  const ModuleData* module =
      module_manager_->GetModuleByPathAndBuildId(module_path, module_build_id);
  if (module == nullptr) return std::nullopt;

  const uint64_t offset =
      absolute_address - module_base_address + module->executable_segment_offset();
  const auto* function_info = module->FindFunctionByOffset(offset, false);
  if (function_info == nullptr) return std::nullopt;

  return module_base_address - module->load_bias() - module->executable_segment_offset() +
         function_info->address();
}

const FunctionInfo* CaptureData::FindFunctionByModulePathBuildIdAndOffset(
    const std::string& module_path, const std::string& build_id, uint64_t offset) const {
  const ModuleData* module_data = module_manager_->GetModuleByPathAndBuildId(module_path, build_id);
  if (module_data == nullptr) {
    return nullptr;
  }

  uint64_t address = module_data->load_bias() + offset;

  return module_data->FindFunctionByElfAddress(address, true);
}

std::optional<std::string> CaptureData::FindModuleBuildIdByAddress(
    uint64_t absolute_address) const {
  const ModuleData* module_data = FindModuleByAddress(absolute_address);
  if (module_data == nullptr) {
    return std::nullopt;
  }
  return module_data->build_id();
}

const std::string& CaptureData::GetModulePathByAddress(uint64_t absolute_address) const {
  const ModuleData* module_data = FindModuleByAddress(absolute_address);
  if (module_data != nullptr) {
    return module_data->file_path();
  }
  const auto address_info_it = address_infos_.find(absolute_address);
  if (address_info_it == address_infos_.end()) {
    return kUnknownFunctionOrModuleName;
  }
  const LinuxAddressInfo& address_info = address_info_it->second;
  const std::string& module_path = address_info.module_path();
  if (module_path.empty()) {
    return kUnknownFunctionOrModuleName;
  }
  return module_path;
}

const FunctionInfo* CaptureData::FindFunctionByAddress(uint64_t absolute_address,
                                                       bool is_exact) const {
  const auto module_or_error = process_.FindModuleByAddress(absolute_address);
  if (module_or_error.has_error()) return nullptr;
  const auto& module_in_memory = module_or_error.value();
  const std::string& module_path = module_in_memory.file_path();
  const std::string& module_build_id = module_in_memory.build_id();
  const uint64_t module_base_address = module_in_memory.start();

  const ModuleData* module =
      module_manager_->GetModuleByPathAndBuildId(module_path, module_build_id);
  if (module == nullptr) return nullptr;

  const uint64_t offset =
      absolute_address - module_base_address + module->executable_segment_offset();
  return module->FindFunctionByOffset(offset, is_exact);
}

[[nodiscard]] ModuleData* CaptureData::FindModuleByAddress(uint64_t absolute_address) const {
  const auto result = process_.FindModuleByAddress(absolute_address);
  if (result.has_error()) return nullptr;
  return module_manager_->GetMutableModuleByPathAndBuildId(result.value().file_path(),
                                                           result.value().build_id());
}

int32_t CaptureData::process_id() const { return process_.pid(); }

std::string CaptureData::process_name() const { return process_.name(); }

void CaptureData::EnableFrameTrack(uint64_t instrumented_function_id) {
  if (frame_track_function_ids_.contains(instrumented_function_id)) {
    const auto* function = GetInstrumentedFunctionById(instrumented_function_id);
    CHECK(function != nullptr);
    LOG("Warning: Frame track for instrumented function \"%s\" is already enabled",
        function->function_name());
    return;
  }
  frame_track_function_ids_.insert(instrumented_function_id);
}

void CaptureData::DisableFrameTrack(uint64_t instrumented_function_id) {
  frame_track_function_ids_.erase(instrumented_function_id);
}

[[nodiscard]] bool CaptureData::IsFrameTrackEnabled(uint64_t instrumented_function_id) const {
  return frame_track_function_ids_.contains(instrumented_function_id);
}

}  // namespace orbit_client_model
