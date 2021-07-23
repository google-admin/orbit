// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_TIME_GRAPH_H_
#define ORBIT_GL_TIME_GRAPH_H_

#include <absl/container/flat_hash_map.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "AccessibleInterfaceProvider.h"
#include "AccessibleTimeGraph.h"
#include "Batcher.h"
#include "CallstackThreadBar.h"
#include "CaptureViewElement.h"
#include "ClientData/TimerChain.h"
#include "ClientModel/CaptureData.h"
#include "CoreMath.h"
#include "ManualInstrumentationManager.h"
#include "OrbitAccessibility/AccessibleInterface.h"
#include "PickingManager.h"
#include "TextRenderer.h"
#include "TimeGraphLayout.h"
#include "Track.h"
#include "TrackManager.h"
#include "Viewport.h"
#include "absl/container/flat_hash_map.h"
#include "capture_data.pb.h"

class OrbitApp;

class TimeGraph : public orbit_gl::CaptureViewElement {
 public:
  explicit TimeGraph(AccessibleInterfaceProvider* parent, OrbitApp* app,
                     orbit_gl::Viewport* viewport,
                     const orbit_client_model::CaptureData* capture_data,
                     PickingManager* picking_manager);
  ~TimeGraph() override;

  void Draw(Batcher& batcher, TextRenderer& text_renderer, uint64_t current_mouse_time_ns,
            PickingMode picking_mode = PickingMode::kNone, float z_offset = 0) override;
  void DrawTracks(Batcher& batcher, TextRenderer& text_renderer, uint64_t current_mouse_time_ns,
                  PickingMode picking_mode = PickingMode::kNone);
  void DrawOverlay(Batcher& batcher, TextRenderer& text_renderer, PickingMode picking_mode);
  void DrawIteratorBox(Batcher& batcher, TextRenderer& text_renderer, Vec2 pos, Vec2 size,
                       const Color& color, const std::string& label, const std::string& time,
                       float text_box_y);
  void DrawIncompleteDataIntervals(Batcher& batcher, PickingMode picking_mode);
  void DrawText(float layer);

  void RequestUpdate() override;
  void UpdatePrimitives(Batcher* /*batcher*/, uint64_t /*min_tick*/, uint64_t /*max_tick*/,
                        PickingMode /*picking_mode*/, float /*z_offset*/ = 0) override;
  void SelectCallstacks(float world_start, float world_end, int32_t thread_id);
  const std::vector<orbit_client_protos::CallstackEvent>& GetSelectedCallstackEvents(int32_t tid);

  void ProcessTimer(const orbit_client_protos::TimerInfo& timer_info,
                    const orbit_grpc_protos::InstrumentedFunction* function);

  [[nodiscard]] const orbit_client_model::CaptureData* GetCaptureData() const {
    return capture_data_;
  }
  [[nodiscard]] TrackManager* GetTrackManager() { return track_manager_.get(); }

  [[nodiscard]] float GetTextBoxHeight() const { return layout_.GetTextBoxHeight(); }
  [[nodiscard]] float GetWorldFromTick(uint64_t time) const;
  [[nodiscard]] float GetWorldFromUs(double micros) const;
  [[nodiscard]] uint64_t GetTickFromWorld(float world_x) const;
  [[nodiscard]] uint64_t GetTickFromUs(double micros) const;
  [[nodiscard]] double GetUsFromTick(uint64_t time) const;
  [[nodiscard]] double GetTimeWindowUs() const { return time_window_us_; }
  void UpdateCaptureMinMaxTimestamps();

  void ZoomAll();
  void Zoom(const orbit_client_protos::TimerInfo& timer_info);
  void Zoom(uint64_t min, uint64_t max);
  void ZoomTime(float zoom_value, double mouse_ratio);
  void VerticalZoom(float zoom_value, float mouse_ratio);
  void SetMinMax(double min_time_us, double max_time_us);
  void PanTime(int initial_x, int current_x, int width, double initial_time);
  enum class VisibilityType {
    kPartlyVisible,
    kFullyVisible,
  };
  void HorizontallyMoveIntoView(VisibilityType vis_type, uint64_t min, uint64_t max,
                                double distance = 0.3);
  void HorizontallyMoveIntoView(VisibilityType vis_type,
                                const orbit_client_protos::TimerInfo& timer_info,
                                double distance = 0.3);
  void VerticallyMoveIntoView(const orbit_client_protos::TimerInfo& timer_info);
  void VerticallyMoveIntoView(Track& track);

  [[nodiscard]] double GetTime(double ratio) const;
  void SelectAndMakeVisible(const orbit_client_protos::TimerInfo* timer_info);
  enum class JumpScope { kSameDepth, kSameThread, kSameFunction, kSameThreadSameFunction };
  enum class JumpDirection { kPrevious, kNext, kTop, kDown };
  void JumpToNeighborTimer(const orbit_client_protos::TimerInfo* from, JumpDirection jump_direction,
                           JumpScope jump_scope);
  [[nodiscard]] const orbit_client_protos::TimerInfo* FindPreviousFunctionCall(
      uint64_t function_address, uint64_t current_time,
      std::optional<int32_t> thread_id = std::nullopt) const;
  [[nodiscard]] const orbit_client_protos::TimerInfo* FindNextFunctionCall(
      uint64_t function_address, uint64_t current_time,
      std::optional<int32_t> thread_id = std::nullopt) const;
  void SelectAndZoom(const orbit_client_protos::TimerInfo* timer_info);
  [[nodiscard]] double GetCaptureTimeSpanUs() const;
  [[nodiscard]] double GetCurrentTimeSpanUs() const;
  void RequestRedraw() { redraw_requested_ = true; }
  [[nodiscard]] bool IsRedrawNeeded() const { return redraw_requested_; }
  void SetThreadFilter(const std::string& filter);

  [[nodiscard]] bool IsFullyVisible(uint64_t min, uint64_t max) const;
  [[nodiscard]] bool IsPartlyVisible(uint64_t min, uint64_t max) const;
  [[nodiscard]] bool IsVisible(VisibilityType vis_type, uint64_t min, uint64_t max) const;

  [[nodiscard]] int GetNumDrawnTextBoxes() const { return num_drawn_text_boxes_; }
  [[nodiscard]] TextRenderer* GetTextRenderer() { return &text_renderer_static_; }
  [[nodiscard]] Batcher& GetBatcher() { return batcher_; }
  [[nodiscard]] std::vector<const orbit_client_data::TimerChain*> GetAllThreadTrackTimerChains()
      const;

  void UpdateHorizontalScroll(float ratio);
  [[nodiscard]] double GetMinTimeUs() const { return min_time_us_; }
  [[nodiscard]] double GetMaxTimeUs() const { return max_time_us_; }
  [[nodiscard]] const TimeGraphLayout& GetLayout() const { return layout_; }
  [[nodiscard]] TimeGraphLayout& GetLayout() { return layout_; }
  [[nodiscard]] float GetRightMargin() const { return right_margin_; }
  void UpdateRightMargin(float margin);

  [[nodiscard]] const orbit_client_protos::TimerInfo* FindPrevious(
      const orbit_client_protos::TimerInfo& from);
  [[nodiscard]] const orbit_client_protos::TimerInfo* FindNext(
      const orbit_client_protos::TimerInfo& from);
  [[nodiscard]] const orbit_client_protos::TimerInfo* FindTop(
      const orbit_client_protos::TimerInfo& from);
  [[nodiscard]] const orbit_client_protos::TimerInfo* FindDown(
      const orbit_client_protos::TimerInfo& from);
  [[nodiscard]] std::pair<const orbit_client_protos::TimerInfo*,
                          const orbit_client_protos::TimerInfo*>
  GetMinMaxTimerInfoForFunction(uint64_t function_id) const;

  [[nodiscard]] static Color GetColor(uint32_t id) {
    constexpr unsigned char kAlpha = 255;
    static std::vector<Color> colors{
        Color(231, 68, 53, kAlpha),    // red
        Color(43, 145, 175, kAlpha),   // blue
        Color(185, 117, 181, kAlpha),  // purple
        Color(87, 166, 74, kAlpha),    // green
        Color(215, 171, 105, kAlpha),  // beige
        Color(248, 101, 22, kAlpha)    // orange
    };
    return colors[id % colors.size()];
  }
  [[nodiscard]] static Color GetColor(uint64_t id) { return GetColor(static_cast<uint32_t>(id)); }
  [[nodiscard]] static Color GetColor(const std::string& str) {
    return GetColor(std::hash<std::string>{}(str));
  }
  [[nodiscard]] static Color GetThreadColor(int32_t tid) {
    return GetColor(static_cast<uint32_t>(tid));
  }

  void SetIteratorOverlayData(
      const absl::flat_hash_map<uint64_t, const orbit_client_protos::TimerInfo*>&
          iterator_timer_info,
      const absl::flat_hash_map<uint64_t, uint64_t>& iterator_id_to_function_id) {
    iterator_timer_info_ = iterator_timer_info;
    iterator_id_to_function_id_ = iterator_id_to_function_id;
    RequestRedraw();
  }

  [[nodiscard]] uint64_t GetCaptureMin() const { return capture_min_timestamp_; }
  [[nodiscard]] uint64_t GetCaptureMax() const { return capture_max_timestamp_; }

  [[nodiscard]] bool HasFrameTrack(uint64_t function_id) const;
  void RemoveFrameTrack(uint64_t function_id);

  [[nodiscard]] AccessibleInterfaceProvider* GetAccessibleParent() const {
    return accessible_parent_;
  }

 protected:
  [[nodiscard]] std::unique_ptr<orbit_accessibility::AccessibleInterface>
  CreateAccessibleInterface() override;

  void ProcessOrbitFunctionTimer(orbit_client_protos::FunctionInfo::OrbitType type,
                                 const orbit_client_protos::TimerInfo& timer_info);
  void ProcessApiEventTimer(const orbit_client_protos::TimerInfo& timer_info);
  void ProcessIntrospectionTimer(const orbit_client_protos::TimerInfo& timer_info);
  void ProcessValueTrackingTimer(const orbit_client_protos::TimerInfo& timer_info);
  void ProcessAsyncTimer(const std::string& track_name,
                         const orbit_client_protos::TimerInfo& timer_info);
  void ProcessSystemMemoryTrackingTimer(const orbit_client_protos::TimerInfo& timer_info);
  void ProcessCGroupAndProcessMemoryTrackingTimer(const orbit_client_protos::TimerInfo& timer_info);
  void ProcessPageFaultsTrackingTimer(const orbit_client_protos::TimerInfo& timer_info);

 private:
  AccessibleInterfaceProvider* accessible_parent_;
  TextRenderer text_renderer_static_;
  int num_drawn_text_boxes_ = 0;

  // First member is id.
  absl::flat_hash_map<uint64_t, const orbit_client_protos::TimerInfo*> iterator_timer_info_;
  absl::flat_hash_map<uint64_t, uint64_t> iterator_id_to_function_id_;

  double ref_time_us_ = 0;
  double min_time_us_ = 0;
  double max_time_us_ = 0;
  uint64_t capture_min_timestamp_ = std::numeric_limits<uint64_t>::max();
  uint64_t capture_max_timestamp_ = 0;
  double time_window_us_ = 0;
  float world_start_x_ = 0;
  float world_width_ = 0;
  float right_margin_ = 0;

  TimeGraphLayout layout_;

  // Be careful when directly changing these members without using the
  // methods RequestRedraw() or RequestUpdatePrimitives():
  // update_primitives_requested_ should always imply redraw_requested_, that is
  // update_primitives_requested_ => redraw_requested_ is an invariant of this
  // class. When updating the primitives, which computes the primitives
  // to be drawn and their coordinates, we always have to redraw the
  // timeline.
  bool update_primitives_requested_ = false;
  bool redraw_requested_ = false;

  bool draw_text_ = true;

  Batcher batcher_;

  std::unique_ptr<TrackManager> track_manager_;

  absl::flat_hash_map<int32_t, std::vector<orbit_client_protos::CallstackEvent>>
      selected_callstack_events_per_thread_;

  ManualInstrumentationManager* manual_instrumentation_manager_;
  std::unique_ptr<ManualInstrumentationManager::AsyncTimerInfoListener> async_timer_info_listener_;
  const orbit_client_model::CaptureData* capture_data_ = nullptr;

  OrbitApp* app_ = nullptr;
};

#endif  // ORBIT_GL_TIME_GRAPH_H_
