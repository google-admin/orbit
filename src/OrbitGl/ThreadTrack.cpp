// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ThreadTrack.h"

#include <absl/strings/str_format.h>
#include <absl/synchronization/mutex.h>
#include <absl/time/time.h>

#include <algorithm>
#include <atomic>
#include <optional>

#include "App.h"
#include "Batcher.h"
#include "ClientData/FunctionUtils.h"
#include "ClientData/TimerChain.h"
#include "ClientModel/CaptureData.h"
#include "DisplayFormats/DisplayFormats.h"
#include "GlUtils.h"
#include "Introspection/Introspection.h"
#include "ManualInstrumentationManager.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/ThreadConstants.h"
#include "TextRenderer.h"
#include "TimeGraph.h"
#include "TimeGraphLayout.h"
#include "TriangleToggle.h"
#include "Viewport.h"
#include "capture_data.pb.h"

using orbit_client_data::TimerChain;
using orbit_client_model::CaptureData;
using orbit_client_protos::FunctionInfo;
using orbit_client_protos::TimerInfo;
using orbit_grpc_protos::InstrumentedFunction;

ThreadTrack::ThreadTrack(CaptureViewElement* parent, TimeGraph* time_graph,
                         orbit_gl::Viewport* viewport, TimeGraphLayout* layout, int32_t thread_id,
                         OrbitApp* app, const CaptureData* capture_data,
                         ScopeTreeUpdateType scope_tree_update_type, uint32_t indentation_level)
    : TimerTrack(parent, time_graph, viewport, layout, app, capture_data, indentation_level) {
  thread_id_ = thread_id;
  InitializeNameAndLabel(thread_id);

  thread_state_bar_ = std::make_shared<orbit_gl::ThreadStateBar>(this, app_, time_graph, viewport,
                                                                 layout, capture_data, thread_id_);

  event_bar_ = std::make_shared<orbit_gl::CallstackThreadBar>(this, app_, time_graph, viewport,
                                                              layout, capture_data, thread_id_);
  event_bar_->SetThreadId(thread_id);

  tracepoint_bar_ = std::make_shared<orbit_gl::TracepointThreadBar>(
      this, app_, time_graph, viewport, layout, capture_data, thread_id_);
  SetTrackColor(TimeGraph::GetThreadColor(thread_id));

  scope_tree_update_type_ = scope_tree_update_type;
}

std::string ThreadTrack::GetThreadNameFromTid(uint32_t thread_id) {
  return capture_data_->GetThreadName(thread_id);
}

void ThreadTrack::InitializeNameAndLabel(int32_t thread_id) {
  if (thread_id == orbit_base::kAllThreadsOfAllProcessesTid) {
    SetName("All tracepoint events");
    SetLabel("All tracepoint events");
  } else if (thread_id == orbit_base::kAllProcessThreadsTid) {
    // This is the process track.
    std::string process_name = capture_data_->process_name();
    SetName("All Threads");
    const std::string_view all_threads = " (all_threads)";
    SetLabel(process_name.append(all_threads));
    SetNumberOfPrioritizedTrailingCharacters(all_threads.size() - 1);
  } else {
    const std::string& thread_name = GetThreadNameFromTid(thread_id);
    SetName(thread_name);
    std::string tid_str = std::to_string(thread_id);
    std::string track_label = absl::StrFormat("%s [%s]", thread_name, tid_str);
    SetLabel(track_label);
    SetNumberOfPrioritizedTrailingCharacters(tid_str.size() + 2);
  }
}

const TimerInfo* ThreadTrack::GetLeft(const TimerInfo& timer_info) const {
  if (timer_info.thread_id() == thread_id_) {
    const TimerChain* chain = track_data_->GetChain(timer_info.depth());
    if (chain != nullptr) return chain->GetElementBefore(timer_info);
  }
  return nullptr;
}

const TimerInfo* ThreadTrack::GetRight(const TimerInfo& timer_info) const {
  if (timer_info.thread_id() == thread_id_) {
    const TimerChain* chain = track_data_->GetChain(timer_info.depth());
    if (chain != nullptr) return chain->GetElementAfter(timer_info);
  }
  return nullptr;
}

std::string ThreadTrack::GetBoxTooltip(const Batcher& batcher, PickingId id) const {
  const TimerInfo* timer_info = batcher.GetTimerInfo(id);
  if (timer_info == nullptr || timer_info->type() == TimerInfo::kCoreActivity) {
    return "";
  }

  const InstrumentedFunction* func =
      capture_data_->GetInstrumentedFunctionById(timer_info->function_id());

  FunctionInfo::OrbitType type{FunctionInfo::kNone};
  if (func != nullptr) {
    type = orbit_client_data::function_utils::GetOrbitTypeByName(func->function_name());
  }

  std::string function_name;
  bool is_manual = (func != nullptr && type == FunctionInfo::kOrbitTimerStart) ||
                   timer_info->type() == TimerInfo::kApiEvent;

  if (!func && !is_manual) {
    return GetTimesliceText(*timer_info);
  }

  if (is_manual) {
    auto api_event = ManualInstrumentationManager::ApiEventFromTimerInfo(*timer_info);
    function_name = api_event.name;
  } else {
    function_name = func->function_name();
  }

  std::string module_name =
      func != nullptr
          ? orbit_client_data::function_utils::GetLoadedModuleNameByPath(func->file_path())
          : "unknown";

  return absl::StrFormat(
      "<b>%s</b><br/>"
      "<i>Timing measured through %s instrumentation</i>"
      "<br/><br/>"
      "<b>Module:</b> %s<br/>"
      "<b>Time:</b> %s",
      function_name, is_manual ? "manual" : "dynamic", module_name,
      orbit_display_formats::GetDisplayTime(
          TicksToDuration(timer_info->start(), timer_info->end())));
}

bool ThreadTrack::IsTimerActive(const TimerInfo& timer_info) const {
  return timer_info.type() == TimerInfo::kIntrospection ||
         timer_info.type() == TimerInfo::kApiEvent ||
         app_->IsFunctionVisible(timer_info.function_id());
}

bool ThreadTrack::IsTrackSelected() const {
  return thread_id_ != orbit_base::kAllProcessThreadsTid &&
         app_->selected_thread_id() == thread_id_;
}

[[nodiscard]] static inline Color ToColor(uint64_t val) {
  return Color((val >> 24) & 0xFF, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
}

[[nodiscard]] static std::optional<Color> GetUserColor(const TimerInfo& timer_info,
                                                       const InstrumentedFunction* function) {
  FunctionInfo::OrbitType type{FunctionInfo::kNone};
  if (function != nullptr) {
    type = orbit_client_data::function_utils::GetOrbitTypeByName(function->function_name());
  }

  bool manual_instrumentation_timer =
      (type == FunctionInfo::kOrbitTimerStart || type == FunctionInfo::kOrbitTimerStartAsync ||
       timer_info.type() == TimerInfo::kApiEvent);

  if (!manual_instrumentation_timer) {
    return std::nullopt;
  }

  orbit_api::Event event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);
  if (event.color == kOrbitColorAuto) {
    return std::nullopt;
  }

  return ToColor(static_cast<uint64_t>(event.color));
}

Color ThreadTrack::GetTimerColor(const TimerInfo& timer_info, const internal::DrawData& draw_data) {
  uint64_t function_id = timer_info.function_id();
  bool is_selected = &timer_info == draw_data.selected_timer;
  bool is_highlighted = !is_selected && function_id != orbit_grpc_protos::kInvalidFunctionId &&
                        function_id == draw_data.highlighted_function_id;
  return GetTimerColor(timer_info, is_selected, is_highlighted);
}

Color ThreadTrack::GetTimerColor(const TimerInfo& timer_info, bool is_selected,
                                 bool is_highlighted) const {
  const Color kInactiveColor(100, 100, 100, 255);
  const Color kSelectionColor(0, 128, 255, 255);
  if (is_highlighted) {
    return TimerTrack::kHighlightColor;
  }
  if (is_selected) {
    return kSelectionColor;
  }
  if (!IsTimerActive(timer_info)) {
    return kInactiveColor;
  }

  uint64_t function_id = timer_info.function_id();
  const InstrumentedFunction* instrumented_function = app_->GetInstrumentedFunction(function_id);
  CHECK(instrumented_function != nullptr || timer_info.type() == TimerInfo::kIntrospection ||
        timer_info.type() == TimerInfo::kApiEvent);
  std::optional<Color> user_color = GetUserColor(timer_info, instrumented_function);

  Color color = kInactiveColor;
  if (user_color.has_value()) {
    color = user_color.value();
  } else if (timer_info.type() == TimerInfo::kIntrospection) {
    orbit_api::Event event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);
    color = event.color == kOrbitColorAuto ? TimeGraph::GetColor(event.name)
                                           : ToColor(static_cast<uint64_t>(event.color));
  } else {
    color = TimeGraph::GetThreadColor(timer_info.thread_id());
  }

  constexpr uint8_t kOddAlpha = 210;
  if (!(timer_info.depth() & 0x1)) {
    color[3] = kOddAlpha;
  }

  return color;
}

void ThreadTrack::UpdateBoxHeight() {
  box_height_ = layout_->GetTextBoxHeight();
  if (collapse_toggle_->IsCollapsed() && depth_ > 0) {
    box_height_ /= static_cast<float>(depth_);
  }
}

bool ThreadTrack::IsEmpty() const {
  return thread_state_bar_->IsEmpty() && event_bar_->IsEmpty() && tracepoint_bar_->IsEmpty() &&
         track_data_->IsEmpty();
}

void ThreadTrack::UpdatePositionOfSubtracks() {
  const float thread_state_track_height = layout_->GetThreadStateTrackHeight();
  const float event_track_height = layout_->GetEventTrackHeightFromTid(thread_id_);
  const float space_between_subtracks = layout_->GetSpaceBetweenTracksAndThread();

  float current_y = pos_[1] - layout_->GetTrackTabHeight();

  thread_state_bar_->SetPos(pos_[0], current_y);
  if (!thread_state_bar_->IsEmpty()) {
    current_y -= (space_between_subtracks + thread_state_track_height);
  }

  event_bar_->SetPos(pos_[0], current_y);
  if (!event_bar_->IsEmpty()) {
    current_y -= (space_between_subtracks + event_track_height);
  }

  tracepoint_bar_->SetPos(pos_[0], current_y);
}

void ThreadTrack::UpdateMinMaxTimestamps() {
  track_data_->UpdateMinTime(capture_data_->GetCallstackData()->min_time());
  track_data_->UpdateMaxTime(capture_data_->GetCallstackData()->max_time());
}

void ThreadTrack::Draw(Batcher& batcher, TextRenderer& text_renderer,
                       uint64_t current_mouse_time_ns, PickingMode picking_mode, float z_offset) {
  TimerTrack::Draw(batcher, text_renderer, current_mouse_time_ns, picking_mode, z_offset);

  UpdateMinMaxTimestamps();
  UpdatePositionOfSubtracks();

  const float thread_state_track_height = layout_->GetThreadStateTrackHeight();
  const float event_track_height = layout_->GetEventTrackHeightFromTid(thread_id_);
  const float tracepoint_track_height = layout_->GetEventTrackHeightFromTid(thread_id_);
  const float track_width = size_[0];

  if (!thread_state_bar_->IsEmpty()) {
    thread_state_bar_->SetSize(track_width, thread_state_track_height);
    thread_state_bar_->Draw(batcher, text_renderer, current_mouse_time_ns, picking_mode, z_offset);
  }

  if (!event_bar_->IsEmpty()) {
    event_bar_->SetSize(track_width, event_track_height);
    event_bar_->Draw(batcher, text_renderer, current_mouse_time_ns, picking_mode, z_offset);
  }

  if (!tracepoint_bar_->IsEmpty()) {
    tracepoint_bar_->SetSize(track_width, tracepoint_track_height);
    tracepoint_bar_->Draw(batcher, text_renderer, current_mouse_time_ns, picking_mode, z_offset);
  }
}

void ThreadTrack::OnPick(int x, int y) {
  Track::OnPick(x, y);
  app_->set_selected_thread_id(thread_id_);
}

std::vector<orbit_gl::CaptureViewElement*> ThreadTrack::GetVisibleChildren() {
  std::vector<CaptureViewElement*> result;
  if (!thread_state_bar_->IsEmpty()) {
    result.push_back(thread_state_bar_.get());
  }

  if (!event_bar_->IsEmpty()) {
    result.push_back(event_bar_.get());
  }

  if (!tracepoint_bar_->IsEmpty()) {
    result.push_back(tracepoint_bar_.get());
  }

  return result;
}

void ThreadTrack::SetTrackColor(Color color) {
  absl::MutexLock lock(&mutex_);
  event_bar_->SetColor(color);
  tracepoint_bar_->SetColor(color);
}

std::string ThreadTrack::GetTimesliceText(const TimerInfo& timer_info) const {
  std::string time = GetDisplayTime(timer_info);

  const InstrumentedFunction* func = app_->GetInstrumentedFunction(timer_info.function_id());
  if (func != nullptr) {
    std::string extra_info = GetExtraInfo(timer_info);
    std::string name;
    if (func->function_type() == InstrumentedFunction::kTimerStart) {
      auto api_event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);
      name = api_event.name;
    } else {
      name = func->function_name();
    }

    return absl::StrFormat("%s %s %s", name, extra_info.c_str(), time);
  } else if (timer_info.type() == TimerInfo::kIntrospection) {
    auto api_event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);
    return absl::StrFormat("%s %s", api_event.name, time);
  } else if (timer_info.type() == TimerInfo::kApiEvent) {
    auto api_event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);
    std::string extra_info = GetExtraInfo(timer_info);
    return absl::StrFormat("%s %s %s", api_event.name, extra_info.c_str(), time);
  } else {
    ERROR(
        "Unexpected case in ThreadTrack::SetTimesliceText, function=\"%s\", "
        "type=%d",
        func->function_name(), static_cast<int>(timer_info.type()));
  }
  return "";
}

std::string ThreadTrack::GetTooltip() const {
  if (thread_id_ == orbit_base::kAllProcessThreadsTid) {
    return "Shows collected samples for all threads of process " + capture_data_->process_name() +
           " " + std::to_string(capture_data_->process_id());
  } else {
    return "Shows collected samples and timings from dynamically instrumented "
           "functions";
  }
}

float ThreadTrack::GetHeight() const {
  const uint32_t depth =
      collapse_toggle_->IsCollapsed() ? std::min<uint32_t>(1, GetDepth()) : GetDepth();

  bool gap_between_tracks_and_timers =
      (!thread_state_bar_->IsEmpty() || !event_bar_->IsEmpty() || !tracepoint_bar_->IsEmpty()) &&
      (depth > 0);
  return GetHeaderHeight() +
         (gap_between_tracks_and_timers ? layout_->GetSpaceBetweenTracksAndThread() : 0) +
         layout_->GetTextBoxHeight() * depth + layout_->GetTrackBottomMargin();
}

float ThreadTrack::GetHeaderHeight() const {
  const float thread_state_track_height = layout_->GetThreadStateTrackHeight();
  const float event_track_height = layout_->GetEventTrackHeightFromTid(thread_id_);
  const float tracepoint_track_height = layout_->GetEventTrackHeightFromTid(thread_id_);
  const float space_between_subtracks = layout_->GetSpaceBetweenTracksAndThread();

  float header_height = layout_->GetTrackTabHeight();
  int track_count = 0;
  if (!thread_state_bar_->IsEmpty()) {
    header_height += thread_state_track_height;
    ++track_count;
  }

  if (!event_bar_->IsEmpty()) {
    header_height += event_track_height;
    ++track_count;
  }

  if (!tracepoint_bar_->IsEmpty()) {
    header_height += tracepoint_track_height;
    ++track_count;
  }

  header_height += std::max(0, track_count - 1) * space_between_subtracks;
  return header_height;
}

float ThreadTrack::GetYFromDepth(uint32_t depth) const {
  bool gap_between_tracks_and_timers =
      !thread_state_bar_->IsEmpty() || !event_bar_->IsEmpty() || !tracepoint_bar_->IsEmpty();
  return pos_[1] - GetHeaderHeight() -
         (gap_between_tracks_and_timers ? layout_->GetSpaceBetweenTracksAndThread() : 0) -
         box_height_ * static_cast<float>(depth + 1);
}

void ThreadTrack::OnTimer(const TimerInfo& timer_info) {
  UpdateDepth(timer_info.depth() + 1);

  if (process_id_ == -1) {
    process_id_ = timer_info.process_id();
  }

  // Thread tracks use a ScopeTree so we don't need to create one TimerChain per depth.
  // Allocate a single TimerChain into which all timers will be appended.

  // Pass ownership to timer_chain.
  const auto& timer_info_chain_ref = track_data_->AddTimer(/*depth=*/0, timer_info);

  if (scope_tree_update_type_ == ScopeTreeUpdateType::kAlways) {
    absl::MutexLock lock(&scope_tree_mutex_);
    scope_tree_.Insert(&timer_info_chain_ref);
  }
}

void ThreadTrack::OnCaptureComplete() {
  if (scope_tree_update_type_ != ScopeTreeUpdateType::kOnCaptureComplete) {
    return;
  }
  // Build ScopeTree from timer chains.
  std::vector<const TimerChain*> timer_chains = track_data_->GetChains();
  for (const TimerChain* timer_chain : timer_chains) {
    CHECK(timer_chain != nullptr);
    absl::MutexLock lock(&scope_tree_mutex_);
    for (const auto& block : *timer_chain) {
      for (size_t k = 0; k < block.size(); ++k) {
        scope_tree_.Insert(&block[k]);
      }
    }
  }
}

[[nodiscard]] static std::pair<float, float> GetBoxPosXAndWidth(const internal::DrawData& draw_data,
                                                                const TimeGraph* time_graph,
                                                                const TimerInfo& timer_info) {
  double start_us = time_graph->GetUsFromTick(timer_info.start());
  double end_us = time_graph->GetUsFromTick(timer_info.end());
  double elapsed_us = end_us - start_us;
  double normalized_start = start_us * draw_data.inv_time_window;
  double normalized_length = elapsed_us * draw_data.inv_time_window;
  float world_timer_width = static_cast<float>(normalized_length * draw_data.track_width);
  float world_timer_x =
      static_cast<float>(draw_data.track_start_x + normalized_start * draw_data.track_width);
  return {world_timer_x, world_timer_width};
}

[[nodiscard]] static inline uint64_t GetNextPixelBoundaryTimeNs(
    float world_x, const internal::DrawData& draw_data) {
  float normalized_x = (world_x - draw_data.track_start_x) / draw_data.track_width;
  int pixel_x = static_cast<int>(ceil(normalized_x * draw_data.viewport->GetScreenWidth()));
  return draw_data.min_tick + pixel_x * draw_data.ns_per_pixel;
}

// We minimize overdraw when drawing lines for small events by discarding events that would just
// draw over an already drawn pixel line. When zoomed in enough that all events are drawn as boxes,
// this has no effect. When zoomed  out, many events will be discarded quickly.
void ThreadTrack::UpdatePrimitives(Batcher* batcher, uint64_t min_tick, uint64_t max_tick,
                                   PickingMode picking_mode, float z_offset) {
  CHECK(batcher);
  visible_timer_count_ = 0;
  UpdatePrimitivesOfSubtracks(batcher, min_tick, max_tick, picking_mode, z_offset);
  UpdateBoxHeight();

  const internal::DrawData draw_data = GetDrawData(
      min_tick, max_tick, size_[0], z_offset, batcher, time_graph_, viewport_,
      collapse_toggle_->IsCollapsed(), app_->selected_timer(), app_->GetFunctionIdToHighlight());

  absl::MutexLock lock(&scope_tree_mutex_);

  for (const auto& [depth, ordered_nodes] : scope_tree_.GetOrderedNodesByDepth()) {
    auto first_node_to_draw = ordered_nodes.lower_bound(min_tick);
    if (first_node_to_draw != ordered_nodes.begin()) --first_node_to_draw;

    UpdateDepth(depth);
    float world_timer_y = GetYFromDepth(depth - 1);
    uint64_t next_pixel_start_time_ns = min_tick;

    for (auto it = first_node_to_draw; it != ordered_nodes.end() && it->first < max_tick; ++it) {
      const orbit_client_protos::TimerInfo& timer_info = *it->second->GetScope();
      if (timer_info.end() <= next_pixel_start_time_ns) continue;
      ++visible_timer_count_;

      Color color = GetTimerColor(timer_info, draw_data);
      std::unique_ptr<PickingUserData> user_data = CreatePickingUserData(*batcher, timer_info);

      const auto [pos_x, size_x] = GetBoxPosXAndWidth(draw_data, time_graph_, timer_info);
      const Vec2 pos = {pos_x, world_timer_y};
      const Vec2 size = {size_x, box_height_};

      auto timer_duration = timer_info.end() - timer_info.start();
      if (timer_duration > draw_data.ns_per_pixel) {
        if (!collapse_toggle_->IsCollapsed()) {
          DrawTimesliceText(timer_info, draw_data.track_start_x, z_offset, pos, size);
        }
        batcher->AddShadedBox(pos, size, draw_data.z, color, std::move(user_data));
      } else {
        batcher->AddVerticalLine(pos, box_height_, draw_data.z, color, std::move(user_data));
      }

      // Use the time at boundary of the next pixel as a threshold to avoid overdraw.
      next_pixel_start_time_ns = GetNextPixelBoundaryTimeNs(pos[0] + size[0], draw_data);
    }
  }
}

void ThreadTrack::UpdatePrimitivesOfSubtracks(Batcher* batcher, uint64_t min_tick,
                                              uint64_t max_tick, PickingMode picking_mode,
                                              float z_offset) {
  UpdatePositionOfSubtracks();

  if (!thread_state_bar_->IsEmpty()) {
    thread_state_bar_->UpdatePrimitives(batcher, min_tick, max_tick, picking_mode, z_offset);
  }
  if (!event_bar_->IsEmpty()) {
    event_bar_->UpdatePrimitives(batcher, min_tick, max_tick, picking_mode, z_offset);
  }
  if (!tracepoint_bar_->IsEmpty()) {
    tracepoint_bar_->UpdatePrimitives(batcher, min_tick, max_tick, picking_mode, z_offset);
  }
}
