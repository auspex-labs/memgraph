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

#include "storage_service.hpp"

#include <iterator>

#include "spdlog/spdlog.h"
#include "storage/v2/property_value.hpp"
#include "storage/v2/storage.hpp"

namespace {
storage::PropertyValue ThriftValueToPropertyValue(interface::storage::Value &&value) {
  using Type = interface::storage::Value::Type;
  using PropertyValue = storage::PropertyValue;
  using ThriftValue = interface::storage::Value;
  switch (value.getType()) {
    case Type::null_v:
      return PropertyValue{};
    case Type::bool_v:
      return PropertyValue{value.get_bool_v()};
    case Type::int_v:
      return PropertyValue{value.get_int_v()};
    case Type::double_v:
      return PropertyValue{value.get_double_v()};
    case Type::string_v:
      return PropertyValue{value.move_string_v()};
    case Type::list_v: {
      std::vector<PropertyValue> result;
      auto list = value.move_list_v();
      result.resize(list.size());
      std::transform(std::make_move_iterator(list.begin()), std::make_move_iterator(list.end()),
                     std::back_insert_iterator(result), ThriftValueToPropertyValue);
      return PropertyValue{std::move(result)};
    }
    case Type::map_v: {
      std::map<std::string, PropertyValue> result;
      auto map = value.move_map_v();
      std::transform(std::make_move_iterator(map->begin()), std::make_move_iterator(map->end()),
                     std::inserter(result, result.end()),
                     [](std::pair<std::string, ThriftValue> &&p) -> decltype(result)::value_type {
                       return {std::move(p.first), ThriftValueToPropertyValue(std::move(p.second))};
                     });
      return PropertyValue{std::move(result)};
    }
    case Type::date_v:
    case Type::local_time_v:
    case Type::local_date_time_v:
    case Type::duration_v:
    case Type::__EMPTY__:
    case Type::vertex_v:
    case Type::edge_v:
    case Type::path_v:
      return PropertyValue{};
  }
}
}  // namespace
namespace manual::storage {
int64_t StorageServiceHandler::startTransaction() {
  spdlog::info("Starting transaction");
  static std::atomic<int64_t> counter{0};
  const auto transaction_id = ++counter;
  active_transactions_.insert(transaction_id, std::make_shared<::storage::Storage::Accessor>(db_.Access()));
  return transaction_id;
};

void StorageServiceHandler::commitTransaction(::interface::storage::Result &result, int64_t transaction_id) {
  spdlog::info("Commiting transaction");
  if (auto accessor_it = active_transactions_.find(transaction_id); accessor_it != active_transactions_.end()) {
    result.success_ref() = accessor_it->second->Commit().HasError();
  } else {
    result.success_ref() = false;
  }
};

void StorageServiceHandler::abortTransaction(int64_t transaction_id) {
  spdlog::info("Aborting transaction");
  if (auto accessor_it = active_transactions_.find(transaction_id); accessor_it != active_transactions_.end()) {
    accessor_it->second->Abort();
  }
};

void StorageServiceHandler::createVertices(::interface::storage::Result &result,
                                           std::unique_ptr<::interface::storage::CreateVerticesRequest> req) {
  spdlog::info("Creating vertex...");
  result.success_ref() = false;
  auto accessor = active_transactions_.at(req->get_transaction_id());
  const auto &labels_map = req->get_labels_name_map();
  const auto &property_names_map = req->get_property_name_map();
  // auto accessor = db_.Access();

  for (auto &new_vertex : *req->new_vertices_ref()) {
    auto vertex = accessor->CreateVertex();
    for (const auto label_id : new_vertex.get_label_ids()) {
      if (const auto result = vertex.AddLabel(accessor->NameToLabel(labels_map.at(label_id))); result.HasError()) {
        return;
      }
    }

    for (auto &[prop_id, prop] : *new_vertex.properties_ref()) {
      if (const auto result = vertex.SetProperty(accessor->NameToProperty(property_names_map.at(prop_id)),
                                                 ThriftValueToPropertyValue(std::move(prop)));
          result.HasError()) {
        return;
      }
    }
  }

  spdlog::info("Vertex creation done!");
}
}  // namespace manual::storage
