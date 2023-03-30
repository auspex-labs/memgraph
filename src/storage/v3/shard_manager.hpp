// Copyright 2023 Memgraph Ltd.
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

#include <queue>
#include <unordered_map>
#include <variant>
#include <vector>

#include <boost/functional/hash.hpp>
#include <boost/uuid/uuid.hpp>

#include "coordinator/coordinator.hpp"
#include "coordinator/shard_map.hpp"
#include "io/address.hpp"
#include "io/message_conversion.hpp"
#include "io/messages.hpp"
#include "io/rsm/raft.hpp"
#include "io/time.hpp"
#include "io/transport.hpp"
#include "query/v2/requests.hpp"
#include "storage/v3/config.hpp"
#include "storage/v3/shard.hpp"
#include "storage/v3/shard_rsm.hpp"
#include "storage/v3/shard_worker.hpp"
#include "storage/v3/value_conversions.hpp"
#include "utils/variant_helpers.hpp"

namespace memgraph::storage::v3 {

using boost::uuids::uuid;

using coordinator::CoordinatorWriteRequests;
using coordinator::CoordinatorWriteResponses;
using coordinator::HeartbeatRequest;
using coordinator::HeartbeatResponse;
using io::Address;
using io::Duration;
using io::RequestId;
using io::ResponseFuture;
using io::Time;
using io::messages::CoordinatorMessages;
using io::messages::ShardManagerMessages;
using io::messages::ShardMessages;
using io::rsm::Raft;
using io::rsm::WriteRequest;
using io::rsm::WriteResponse;
using msgs::ReadRequests;
using msgs::ReadResponses;
using msgs::WriteRequests;
using msgs::WriteResponses;
using storage::v3::ShardRsm;

using ShardManagerOrRsmMessage = std::variant<ShardMessages, ShardManagerMessages>;
using TimeUuidPair = std::pair<Time, uuid>;

template <typename IoImpl>
using ShardRaft = Raft<IoImpl, ShardRsm, WriteRequests, WriteResponses, ReadRequests, ReadResponses>;

using namespace std::chrono_literals;
static constexpr Duration kMinimumCronInterval = 100ms;
static constexpr Duration kMaximumCronInterval = 200ms;
static_assert(kMinimumCronInterval < kMaximumCronInterval,
              "The minimum cron interval has to be smaller than the maximum cron interval!");

/// The ShardManager is responsible for:
/// * reconciling the storage engine's local configuration with the Coordinator's
///   intentions for how it should participate in multiple raft clusters
/// * replying to heartbeat requests to the Coordinator
/// * routing incoming messages to the appropriate RSM
///
/// Every storage engine has exactly one RsmEngine.
template <typename IoImpl>
class ShardManager {
 public:
  ShardManager(io::Io<IoImpl> io, size_t shard_worker_threads, Address coordinator_leader,
               bool serialize_shard_splits_for_determinisim = false)
      : io_(io),
        coordinator_leader_(coordinator_leader),
        serialize_shard_splits_for_determinisim_(serialize_shard_splits_for_determinisim) {
    MG_ASSERT(shard_worker_threads >= 1);

    for (int i = 0; i < shard_worker_threads; i++) {
      shard_worker::Queue queue;
      shard_worker::ShardWorker worker{io, queue};
      auto worker_handle = std::jthread([worker = std::move(worker)]() mutable { worker.Run(); });

      workers_.emplace_back(queue);
      worker_handles_.emplace_back(std::move(worker_handle));
      worker_rsm_counts_.emplace_back(0);
    }
  }

  ShardManager(ShardManager &&) noexcept = default;
  ShardManager &operator=(ShardManager &&) noexcept = default;
  ShardManager(const ShardManager &) = delete;
  ShardManager &operator=(const ShardManager &) = delete;

  ~ShardManager() {
    for (auto worker : workers_) {
      worker.Push(shard_worker::ShutDown{});
    }

    workers_.clear();

    // The jthread handes for our shard worker threads will be
    // blocked on implicitly when worker_handles_ is destroyed.
  }

  size_t UuidToWorkerIndex(const uuid &to) {
    if (rsm_worker_mapping_.contains(to)) {
      return rsm_worker_mapping_.at(to);
    }

    // We will now create a mapping for this (probably new) shard
    // by choosing the worker with the lowest number of existing
    // mappings.

    size_t min_index = 0;
    size_t min_count = worker_rsm_counts_.at(min_index);

    for (int i = 0; i < worker_rsm_counts_.size(); i++) {
      size_t worker_count = worker_rsm_counts_.at(i);
      if (worker_count <= min_count) {
        min_count = worker_count;
        min_index = i;
      }
    }

    worker_rsm_counts_[min_index]++;
    rsm_worker_mapping_.emplace(to, min_index);

    return min_index;
  }

  void SendToWorkerByIndex(size_t worker_index, shard_worker::Message &&message) {
    workers_[worker_index].Push(std::forward<shard_worker::Message>(message));
  }

  void SendToWorkerByUuid(const uuid &to, shard_worker::Message &&message) {
    size_t worker_index = UuidToWorkerIndex(to);
    SendToWorkerByIndex(worker_index, std::forward<shard_worker::Message>(message));
  }

  /// Periodic protocol maintenance. Returns the time that Cron should be called again
  /// in the future.
  Time Cron() {
    spdlog::trace("ShardManager running Cron, address {}", io_.GetAddress().ToString());
    Time now = io_.Now();

    if (now >= next_reconciliation_) {
      Reconciliation();

      std::uniform_int_distribution time_distrib(kMinimumCronInterval.count(), kMaximumCronInterval.count());

      const auto rand = io_.Rand(time_distrib);

      next_reconciliation_ = now + Duration{rand};
    }

    for (auto &worker : workers_) {
      worker.Push(shard_worker::Cron{});
    }

    Time next_worker_cron = now + std::chrono::milliseconds(500);

    return std::min(next_worker_cron, next_reconciliation_);
  }

  /// Returns the Address for our underlying Io implementation
  Address GetAddress() { return io_.GetAddress(); }

  void InitializeSplitShard(msgs::InitializeSplitShard &&init_split_shard) {
    spdlog::trace("ShardManager received InitializeSplitShard message");
    for (const auto &[from_uuid, new_uuid] : init_split_shard.uuid_mapping) {
      bool has_source = rsm_worker_mapping_.contains(from_uuid);
      if (has_source) {
        const auto low_key = init_split_shard.shard->LowKey();
        coordinator::ShardId new_shard_id = std::make_pair(init_split_shard.shard->PrimaryLabel(), low_key);

        spdlog::debug("ShardManager initialized split shard {} with uuid {} and low key {}",
                      init_split_shard.shard->Version().logical_id, new_uuid, low_key.back());
        msgs::InitializeSplitShardByUUID msg{.shard = std::move(init_split_shard.shard), .shard_uuid = new_uuid};
        SendToWorkerByUuid(new_uuid, std::move(msg));

        initialized_but_not_confirmed_rsm_.emplace(new_uuid, new_shard_id);
        break;
      }
    }
  }

  void Receive(ShardManagerMessages &&smm, RequestId request_id, Address from) {
    std::visit(utils::Overloaded{[this](msgs::SuggestedSplitInfo &&split_info) {
                                   spdlog::debug(
                                       "ShardManager adding new suggested split info to the pending_splits_ structure");
                                   pending_splits_.emplace(std::move(split_info));
                                 },
                                 [this](msgs::InitializeSplitShard &&init_split_shard) {
                                   // TODO(jbajic) remove pending split for this completed split
                                   // TODO(jbajic) Add new shard to initialized but not confirmed rsm
                                   spdlog::debug("ShardManager received a new split shard that it will now initialize");
                                   InitializeSplitShard(std::move(init_split_shard));
                                 }},
               std::move(smm));
  }

  void Route(ShardMessages &&sm, RequestId request_id, Address to, Address from) {
    Address address = io_.GetAddress();

    MG_ASSERT(address.last_known_port == to.last_known_port);
    MG_ASSERT(address.last_known_ip == to.last_known_ip);

    SendToWorkerByUuid(to.unique_id, shard_worker::RouteMessage{
                                         .message = std::move(sm),
                                         .request_id = request_id,
                                         .to = to,
                                         .from = from,
                                     });
  }

  void BlockOnQuiescence() {
    for (const auto &worker : workers_) {
      worker.BlockOnQuiescence();
    }
  }

 private:
  io::Io<IoImpl> io_;
  std::vector<shard_worker::Queue> workers_;
  std::vector<std::jthread> worker_handles_;
  std::vector<size_t> worker_rsm_counts_;
  std::set<msgs::SuggestedSplitInfo> pending_splits_;
  std::unordered_map<uuid, size_t, boost::hash<boost::uuids::uuid>> rsm_worker_mapping_;
  Time next_reconciliation_ = Time::min();
  Address coordinator_leader_;
  std::optional<ResponseFuture<WriteResponse<CoordinatorWriteResponses>>> heartbeat_res_;
  std::map<boost::uuids::uuid, coordinator::ShardId> initialized_but_not_confirmed_rsm_;
  bool serialize_shard_splits_for_determinisim_;

  void Reconciliation() {
    if (heartbeat_res_.has_value()) {
      if (heartbeat_res_->IsReady()) {
        io::ResponseResult<WriteResponse<CoordinatorWriteResponses>> response_result =
            std::move(heartbeat_res_).value().Wait();
        heartbeat_res_.reset();

        if (response_result.HasError()) {
          spdlog::info("ShardManager timed out while trying to reach Coordinator");
        } else {
          auto response_envelope = response_result.GetValue();
          WriteResponse<CoordinatorWriteResponses> wr = response_envelope.message;

          if (wr.retry_leader.has_value()) {
            spdlog::info("ShardManager redirected to new Coordinator leader");
            coordinator_leader_ = wr.retry_leader.value();
          } else if (wr.success) {
            CoordinatorWriteResponses cwr = wr.write_return;
            HeartbeatResponse hr = std::get<HeartbeatResponse>(cwr);
            spdlog::info("ShardManager received heartbeat response from Coordinator");

            EnsureShardsInitialized(hr);
          }
        }
      } else {
        return;
      }
    }

    HeartbeatRequest req{
        .from_storage_manager = GetAddress(),
        .initialized_rsms = initialized_but_not_confirmed_rsm_,
        .suggested_splits = std::move(pending_splits_),
    };

    CoordinatorWriteRequests cwr = req;
    WriteRequest<CoordinatorWriteRequests> ww;
    ww.operation = cwr;

    spdlog::info("ShardManager sending heartbeat to coordinator {} with {} initialized rsms",
                 coordinator_leader_.ToString(), initialized_but_not_confirmed_rsm_.size());
    heartbeat_res_.emplace(std::move(
        io_.template Request<WriteRequest<CoordinatorWriteRequests>, WriteResponse<CoordinatorWriteResponses>>(
            coordinator_leader_, std::move(ww))));
    spdlog::info("SM sent heartbeat");
  }

  void EnsureShardsInitialized(HeartbeatResponse hr) {
    for (const auto &acknowledged_rsm : hr.acknowledged_initialized_rsms) {
      initialized_but_not_confirmed_rsm_.erase(acknowledged_rsm);
    }

    for (const auto &to_init : hr.shards_to_initialize) {
      coordinator::ShardId new_shard_id = std::make_pair(to_init.label_id, to_init.min_key);
      spdlog::trace("ShardManager has been told to initialize shard rsm with uuid {}", to_init.uuid);
      initialized_but_not_confirmed_rsm_.emplace(to_init.uuid, new_shard_id);

      if (rsm_worker_mapping_.contains(to_init.uuid)) {
        spdlog::info(
            "ShardManager forwarding shard intialization request to worker despite a mapping already existing. This "
            "can happen due to benign race conditions.");
      }

      size_t worker_index = UuidToWorkerIndex(to_init.uuid);

      SendToWorkerByIndex(worker_index, to_init);

      rsm_worker_mapping_.emplace(to_init.uuid, worker_index);
    }

    for (const auto &to_split : hr.shards_to_split) {
      for (const auto &[source, destination] : to_split.uuid_mapping) {
        if (rsm_worker_mapping_.contains(source)) {
          // Create the proper layered request providing a Raft write
          // request to the local shard rsm, under the guess that it is
          // the current leader. Most of the time this will be an incorrect
          // guess, but it will eventually succeed when the right ShardManager
          // happens to send the message to the leader shard that is local.
          // This is done to avoid blocking on an RsmClient or maintaining
          // complex async request logic. It's fine to fire-and-forget because
          // it is rare and will eventually succeed.
          msgs::WriteRequests split_request_1 =
              msgs::SplitRequest{.split_key = conversions::ConvertValueVector(to_split.split_key),
                                 .old_shard_version = to_split.old_shard_version,
                                 .new_lhs_shard_version = to_split.new_lhs_shard_version,
                                 .new_rhs_shard_version = to_split.new_rhs_shard_version,
                                 .uuid_mapping = to_split.uuid_mapping};

          WriteRequest<msgs::WriteRequests> split_request_2;
          split_request_2.operation = split_request_1;
          ShardMessages split_request_3 = split_request_2;

          const Address our_address = io_.GetAddress();
          Address shard_address = our_address;
          shard_address.unique_id = source;

          shard_worker::RouteMessage shard_worker_message = {
              .message = split_request_3,
              .request_id = 0,
              .to = shard_address,
              .from = our_address,
          };

          SendToWorkerByUuid(source, shard_worker_message);

          if (serialize_shard_splits_for_determinisim_) {
            // This is only serialized during simulation for determinism
            // purposes, and has been tested for correctness without the
            // imposed determinism.
            size_t worker_index = UuidToWorkerIndex(source);
            workers_[worker_index].BlockOnQuiescence();
          }
        } else {
          MG_ASSERT(false, "bad split source: {}", source);
        }
      }
    }
  }
};

}  // namespace memgraph::storage::v3
