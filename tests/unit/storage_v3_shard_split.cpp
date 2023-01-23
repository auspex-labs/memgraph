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

#include <cstdint>

#include <gmock/gmock-matchers.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "query/v2/requests.hpp"
#include "storage/v3/delta.hpp"
#include "storage/v3/id_types.hpp"
#include "storage/v3/key_store.hpp"
#include "storage/v3/mvcc.hpp"
#include "storage/v3/property_value.hpp"
#include "storage/v3/shard.hpp"
#include "storage/v3/vertex.hpp"
#include "storage/v3/vertex_id.hpp"

using testing::Pair;
using testing::UnorderedElementsAre;

namespace memgraph::storage::v3::tests {

class ShardSplitTest : public testing::Test {
 protected:
  void SetUp() override {
    storage.StoreMapping(
        {{1, "label"}, {2, "property"}, {3, "edge_property"}, {4, "secondary_label"}, {5, "secondary_prop"}});
  }

  const PropertyId primary_property{PropertyId::FromUint(2)};
  const PropertyId secondary_property{PropertyId::FromUint(5)};
  std::vector<storage::v3::SchemaProperty> schema_property_vector = {
      storage::v3::SchemaProperty{primary_property, common::SchemaType::INT}};
  const std::vector<PropertyValue> min_pk{PropertyValue{0}};
  const LabelId primary_label{LabelId::FromUint(1)};
  const LabelId secondary_label{LabelId::FromUint(4)};
  const EdgeTypeId edge_type_id{EdgeTypeId::FromUint(3)};
  Shard storage{primary_label, min_pk, std::nullopt /*max_primary_key*/, schema_property_vector};

  coordinator::Hlc last_hlc{0, io::Time{}};

  coordinator::Hlc GetNextHlc() {
    ++last_hlc.logical_id;
    last_hlc.coordinator_wall_clock += std::chrono::seconds(1);
    return last_hlc;
  }
};

void AssertEqVertexContainer(const VertexContainer &expected, const VertexContainer &actual) {
  ASSERT_EQ(expected.size(), actual.size());

  auto expected_it = expected.begin();
  auto actual_it = actual.begin();
  while (expected_it != expected.end()) {
    EXPECT_EQ(expected_it->first, actual_it->first);
    EXPECT_EQ(expected_it->second.deleted, actual_it->second.deleted);
    EXPECT_EQ(expected_it->second.in_edges, actual_it->second.in_edges);
    EXPECT_EQ(expected_it->second.out_edges, actual_it->second.out_edges);
    EXPECT_EQ(expected_it->second.labels, actual_it->second.labels);

    auto *expected_delta = expected_it->second.delta;
    auto *actual_delta = actual_it->second.delta;
    while (expected_delta != nullptr) {
      EXPECT_EQ(expected_delta->action, actual_delta->action);
      switch (expected_delta->action) {
        case Delta::Action::ADD_LABEL:
        case Delta::Action::REMOVE_LABEL: {
          EXPECT_EQ(expected_delta->label, actual_delta->label);
          break;
        }
        case Delta::Action::SET_PROPERTY: {
          EXPECT_EQ(expected_delta->property.key, actual_delta->property.key);
          EXPECT_EQ(expected_delta->property.value, actual_delta->property.value);
          break;
        }
        case Delta::Action::ADD_IN_EDGE:
        case Delta::Action::ADD_OUT_EDGE:
        case Delta::Action::REMOVE_IN_EDGE:
        case Delta::Action::RECREATE_OBJECT:
        case Delta::Action::DELETE_OBJECT:
        case Delta::Action::REMOVE_OUT_EDGE: {
          break;
        }
      }
      expected_delta = expected_delta->next;
      actual_delta = actual_delta->next;
    }
    EXPECT_EQ(expected_delta, nullptr);
    EXPECT_EQ(actual_delta, nullptr);
    ++expected_it;
    ++actual_it;
  }
}

void AddDeltaToDeltaChain(Vertex *object, Delta *new_delta) {
  auto *delta_holder = GetDeltaHolder(object);

  new_delta->next = delta_holder->delta;
  new_delta->prev.Set(object);
  if (delta_holder->delta) {
    delta_holder->delta->prev.Set(new_delta);
  }
  delta_holder->delta = new_delta;
}

TEST_F(ShardSplitTest, TestBasicSplitWithVertices) {
  auto acc = storage.Access(GetNextHlc());
  EXPECT_FALSE(acc.CreateVertexAndValidate({secondary_label}, {PropertyValue(1)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(2)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(3)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(4)}, {}).HasError());
  EXPECT_FALSE(
      acc.CreateVertexAndValidate({secondary_label}, {PropertyValue(5)}, {{secondary_property, PropertyValue(121)}})
          .HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(6)}, {}).HasError());
  auto current_hlc = GetNextHlc();
  acc.Commit(current_hlc);

  auto splitted_data = storage.PerformSplit({PropertyValue(4)});
  EXPECT_EQ(splitted_data.vertices.size(), 3);
  EXPECT_EQ(splitted_data.edges->size(), 0);
  EXPECT_EQ(splitted_data.transactions.size(), 1);
  EXPECT_EQ(splitted_data.label_indices.size(), 0);
  EXPECT_EQ(splitted_data.label_property_indices.size(), 0);

  CommitInfo commit_info{.start_or_commit_timestamp = current_hlc};
  Delta delta_delete1{Delta::DeleteObjectTag{}, &commit_info, 1};
  Delta delta_delete2{Delta::DeleteObjectTag{}, &commit_info, 2};
  Delta delta_delete3{Delta::DeleteObjectTag{}, &commit_info, 3};
  Delta delta_add_label{Delta::RemoveLabelTag{}, secondary_label, &commit_info, 4};
  Delta delta_add_property{Delta::SetPropertyTag{}, secondary_property, PropertyValue(), &commit_info, 4};
  VertexContainer expected_vertices;
  expected_vertices.emplace(PrimaryKey{PropertyValue{4}}, VertexData(&delta_delete1));
  auto [it, inserted] = expected_vertices.emplace(PrimaryKey{PropertyValue{5}}, VertexData(&delta_delete2));
  expected_vertices.emplace(PrimaryKey{PropertyValue{6}}, VertexData(&delta_delete3));
  it->second.labels.push_back(secondary_label);
  AddDeltaToDeltaChain(&*it, &delta_add_property);
  AddDeltaToDeltaChain(&*it, &delta_add_label);

  AssertEqVertexContainer(expected_vertices, splitted_data.vertices);
}

TEST_F(ShardSplitTest, TestBasicSplitVerticesAndEdges) {
  auto acc = storage.Access(GetNextHlc());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(1)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(2)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(3)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(4)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(5)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(6)}, {}).HasError());

  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(1)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(2)}}, edge_type_id, Gid::FromUint(0))
                   .HasError());
  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(1)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(5)}}, edge_type_id, Gid::FromUint(1))
                   .HasError());
  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(4)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(6)}}, edge_type_id, Gid::FromUint(2))
                   .HasError());

  acc.Commit(GetNextHlc());

  auto splitted_data = storage.PerformSplit({PropertyValue(4)});
  EXPECT_EQ(splitted_data.vertices.size(), 3);
  EXPECT_EQ(splitted_data.edges->size(), 2);
  EXPECT_EQ(splitted_data.transactions.size(), 1);
  EXPECT_EQ(splitted_data.label_indices.size(), 0);
  EXPECT_EQ(splitted_data.label_property_indices.size(), 0);
}

TEST_F(ShardSplitTest, TestBasicSplitBeforeCommit) {
  auto acc = storage.Access(GetNextHlc());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(1)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(2)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(3)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(4)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(5)}, {}).HasError());
  EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(6)}, {}).HasError());

  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(1)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(2)}}, edge_type_id, Gid::FromUint(0))
                   .HasError());
  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(1)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(5)}}, edge_type_id, Gid::FromUint(1))
                   .HasError());
  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(4)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(6)}}, edge_type_id, Gid::FromUint(2))
                   .HasError());

  auto splitted_data = storage.PerformSplit({PropertyValue(4)});
  EXPECT_EQ(splitted_data.vertices.size(), 3);
  EXPECT_EQ(splitted_data.edges->size(), 2);
  EXPECT_EQ(splitted_data.transactions.size(), 1);
  EXPECT_EQ(splitted_data.label_indices.size(), 0);
  EXPECT_EQ(splitted_data.label_property_indices.size(), 0);
}

TEST_F(ShardSplitTest, TestBasicSplitWithCommitedAndOngoingTransactions) {
  {
    auto acc = storage.Access(GetNextHlc());
    EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(1)}, {}).HasError());
    EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(2)}, {}).HasError());
    EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(3)}, {}).HasError());
    EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(4)}, {}).HasError());
    EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(5)}, {}).HasError());
    EXPECT_FALSE(acc.CreateVertexAndValidate({}, {PropertyValue(6)}, {}).HasError());

    acc.Commit(GetNextHlc());
  }
  auto acc = storage.Access(GetNextHlc());
  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(1)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(2)}}, edge_type_id, Gid::FromUint(0))
                   .HasError());
  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(1)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(5)}}, edge_type_id, Gid::FromUint(1))
                   .HasError());
  EXPECT_FALSE(acc.CreateEdge(VertexId{primary_label, PrimaryKey{PropertyValue(4)}},
                              VertexId{primary_label, PrimaryKey{PropertyValue(6)}}, edge_type_id, Gid::FromUint(2))
                   .HasError());

  auto splitted_data = storage.PerformSplit({PropertyValue(4)});
  EXPECT_EQ(splitted_data.vertices.size(), 3);
  EXPECT_EQ(splitted_data.edges->size(), 2);
  EXPECT_EQ(splitted_data.transactions.size(), 2);
  EXPECT_EQ(splitted_data.label_indices.size(), 0);
  EXPECT_EQ(splitted_data.label_property_indices.size(), 0);
}

}  // namespace memgraph::storage::v3::tests