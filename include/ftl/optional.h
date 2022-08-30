/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <ftl/details/optional.h>

namespace android::ftl {

// Superset of std::optional<T> with monadic operations, as proposed in https://wg21.link/P0798R8.
//
// TODO: Remove in C++23.
//
template <typename T>
struct Optional final : std::optional<T> {
  using std::optional<T>::optional;

  // Implicit downcast.
  Optional(std::optional<T> other) : std::optional<T>(std::move(other)) {}

  using std::optional<T>::has_value;
  using std::optional<T>::value;

  // Returns Optional<U> where F is a function that maps T to U.
  template <typename F>
  constexpr auto transform(F&& f) const& {
    using R = details::transform_result_t<F, decltype(value())>;
    if (has_value()) return R(std::invoke(std::forward<F>(f), value()));
    return R();
  }

  template <typename F>
  constexpr auto transform(F&& f) & {
    using R = details::transform_result_t<F, decltype(value())>;
    if (has_value()) return R(std::invoke(std::forward<F>(f), value()));
    return R();
  }

  template <typename F>
  constexpr auto transform(F&& f) const&& {
    using R = details::transform_result_t<F, decltype(std::move(value()))>;
    if (has_value()) return R(std::invoke(std::forward<F>(f), std::move(value())));
    return R();
  }

  template <typename F>
  constexpr auto transform(F&& f) && {
    using R = details::transform_result_t<F, decltype(std::move(value()))>;
    if (has_value()) return R(std::invoke(std::forward<F>(f), std::move(value())));
    return R();
  }

  // Returns Optional<U> where F is a function that maps T to Optional<U>.
  template <typename F>
  constexpr auto and_then(F&& f) const& {
    using R = details::and_then_result_t<F, decltype(value())>;
    if (has_value()) return std::invoke(std::forward<F>(f), value());
    return R();
  }

  template <typename F>
  constexpr auto and_then(F&& f) & {
    using R = details::and_then_result_t<F, decltype(value())>;
    if (has_value()) return std::invoke(std::forward<F>(f), value());
    return R();
  }

  template <typename F>
  constexpr auto and_then(F&& f) const&& {
    using R = details::and_then_result_t<F, decltype(std::move(value()))>;
    if (has_value()) return std::invoke(std::forward<F>(f), std::move(value()));
    return R();
  }

  template <typename F>
  constexpr auto and_then(F&& f) && {
    using R = details::and_then_result_t<F, decltype(std::move(value()))>;
    if (has_value()) return std::invoke(std::forward<F>(f), std::move(value()));
    return R();
  }
};

// Deduction guides.
template <typename T>
Optional(T) -> Optional<T>;

template <typename T>
Optional(std::optional<T>) -> Optional<T>;

}  // namespace android::ftl
