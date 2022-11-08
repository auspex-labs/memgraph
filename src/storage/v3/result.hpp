// Copyright 2022 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#pragma once

#include <type_traits>

#include "utils/result.hpp"

namespace memgraph::storage::v3 {

static_assert(std::is_same_v<uint8_t, unsigned char>);

enum class Error : uint8_t {
  SERIALIZATION_ERROR,
  NONEXISTENT_OBJECT,
  DELETED_OBJECT,
  VERTEX_HAS_EDGES,
  PROPERTIES_DISABLED,
  VERTEX_ALREADY_INSERTED
};

template <class TValue>
using Result = utils::BasicResult<Error, TValue>;

}  // namespace memgraph::storage::v3
