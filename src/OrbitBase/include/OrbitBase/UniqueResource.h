// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_BASE_UNIQUE_RESOURCE_H_
#define ORBIT_BASE_UNIQUE_RESOURCE_H_

#include <functional>
#include <optional>

namespace orbit_base {

/* unique_resource helps to manage a unique identity which is not a pointer,
   so unique_ptr cannot be used. Typical examples are file or window handles
   from C libraries. The resource (handle) is typically small and trivially
   copyable, but that's not required. Though we require it to be noexcept
   moveable.

   Unlike unique_ptr this resource requires you to provide a Deleter type.
   No default deleter exists.

   Empty base class optimization for Deleter is not implemented (yet).
*/
template <typename Resource_, typename Deleter_>
class unique_resource {
 public:
  using Resource = Resource_;
  using Deleter = Deleter_;

  constexpr unique_resource() = default;
  constexpr unique_resource(Resource resource, Deleter deleter = Deleter{})
      : resource_(std::move(resource)), deleter_(std::move(deleter)) {}

  unique_resource(const unique_resource&) = delete;
  unique_resource& operator=(const unique_resource&) = delete;

  unique_resource(unique_resource&& other)
      : resource_(std::move(other.resource_)), deleter_(std::move(other.deleter_)) {
    other.resource_ = std::nullopt;
  }
  unique_resource& operator=(unique_resource&& other) noexcept {
    if (&other != this) {
      RunDeleter();

      get_deleter() = std::move(other.get_deleter());
      resource_ = std::move(other.resource_);
      other.resource_ = std::nullopt;
    }

    return *this;
  }

  ~unique_resource() noexcept { RunDeleter(); }

  Resource get() const noexcept { return resource_.value(); }
  Resource& get_mutable() noexcept { return resource_.value(); }
  Deleter& get_deleter() noexcept { return deleter_; }
  const Deleter& get_deleter() const noexcept { return deleter_; }

  void release() noexcept { resource_ = std::nullopt; }

  explicit operator bool() const noexcept { return resource_.has_value(); }

  void reset(Resource resource) {
    RunDeleter();
    resource_ = std::move(resource);
  }

 private:
  void RunDeleter() {
    if (resource_) {
      std::invoke(get_deleter(), resource_.value());
    }
  }

  std::optional<Resource> resource_;
  Deleter deleter_;
};

template <typename Resource, typename Deleter>
unique_resource(Resource, Deleter)
    ->unique_resource<std::decay_t<Resource>, std::remove_cv_t<Deleter>>;

}  // namespace orbit_base

#endif  // ORBIT_BASE_UNIQUE_RESOURCE_H_
#
