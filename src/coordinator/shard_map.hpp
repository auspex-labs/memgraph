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

#include <map>
#include <vector>

#include "io/address.hpp"

namespace memgraph::coordinator {

enum class Status : uint8_t {
  CONSENSUS_PARTICIPANT,
  INITIALIZING,
  // TODO(tyler) this will possibly have more states,
  // depending on the reconfiguration protocol that we
  // implement.
};

struct AddressAndStatus {
  memgraph::io::Address address;
  Status status;
};

using CompoundKey = std::vector<memgraph::storage::PropertyValue>;
using Shard = std::vector<AddressAndStatus>;
using Shards = std::map<CompoundKey, Shard>;

// TODO(tyler) ask Bendzo or Jure what this type should actually be
using Label = std::string;

class ShardMap {
  uint64_t shard_map_version_;
  std::map<Label, Shards> shards_;

 public:
  Shards GetShardsForRange(Label label, CompoundKey start, CompoundKey end);

  // TODO(tyler) ask Kostas if this would ever actually be useful
  Shard GetShardForKey(Label label, CompoundKey key);

  /// This splits the previous shard
  bool SplitShard(uint64_t previous_shard_map_version, Label label, CompoundKey split_key);
};

}  // namespace memgraph::coordinator