// Copyright 2021 Memgraph Ltd.
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
#include <atomic>
#include <optional>
#include <span>
#include <thread>

#include <pulsar/Client.h>

namespace integrations::pulsar {

namespace pulsar_client = ::pulsar;

class Consumer;

class Message final {
 public:
  explicit Message(pulsar_client::Message &&message);

  std::span<const char> Payload() const;

 private:
  pulsar_client::Message message_;

  friend Consumer;
};

using ConsumerFunction = std::function<void(const std::vector<Message> &)>;

struct ConsumerInfo {
  std::optional<int64_t> batch_size;
  std::optional<std::chrono::milliseconds> batch_interval;
  std::string topic;
  std::string consumer_name;
  std::string service_url;
};

class Consumer final {
 public:
  Consumer(ConsumerInfo info, ConsumerFunction consumer_function);
  ~Consumer();

  Consumer(const Consumer &) = delete;
  Consumer(Consumer &&) noexcept = delete;
  Consumer &operator=(const Consumer &) = delete;
  Consumer &operator=(Consumer &&) = delete;

  bool IsRunning() const;
  void Start();
  void Stop();
  void StopIfRunning();

  void Check(std::optional<std::chrono::milliseconds> timeout, std::optional<int64_t> limit_batches,
             const ConsumerFunction &check_consumer_function) const;

  const ConsumerInfo &Info() const;

 private:
  void StartConsuming();
  void StopConsuming();

  ConsumerInfo info_;
  std::optional<pulsar_client::Client> client_;
  mutable pulsar_client::Consumer consumer_;
  ConsumerFunction consumer_function_;

  mutable std::atomic<bool> is_running_{false};
  mutable std::optional<uint64_t> next_message_timestamp_;
  std::thread thread_;
};
}  // namespace integrations::pulsar