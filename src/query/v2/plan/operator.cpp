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

#include "query/v2/plan/operator.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <cppitertools/chain.hpp>
#include <cppitertools/imap.hpp>

#include "common/errors.hpp"
#include "expr/ast/pretty_print_ast_to_original_expression.hpp"
#include "expr/exceptions.hpp"
#include "query/exceptions.hpp"
#include "query/v2/accessors.hpp"
#include "query/v2/bindings/eval.hpp"
#include "query/v2/bindings/symbol_table.hpp"
#include "query/v2/context.hpp"
#include "query/v2/conversions.hpp"
#include "query/v2/db_accessor.hpp"
#include "query/v2/exceptions.hpp"
#include "query/v2/frontend/ast/ast.hpp"
#include "query/v2/multiframe.hpp"
#include "query/v2/path.hpp"
#include "query/v2/plan/scoped_profile.hpp"
#include "query/v2/request_router.hpp"
#include "query/v2/requests.hpp"
#include "storage/v3/conversions.hpp"
#include "storage/v3/property_value.hpp"
#include "utils/algorithm.hpp"
#include "utils/csv_parsing.hpp"
#include "utils/event_counter.hpp"
#include "utils/exceptions.hpp"
#include "utils/fnv.hpp"
#include "utils/likely.hpp"
#include "utils/logging.hpp"
#include "utils/memory.hpp"
#include "utils/message.hpp"
#include "utils/pmr/unordered_map.hpp"
#include "utils/pmr/unordered_set.hpp"
#include "utils/pmr/vector.hpp"
#include "utils/readable_size.hpp"
#include "utils/string.hpp"
#include "utils/temporal.hpp"
#include "utils/variant_helpers.hpp"

using VertexAccessor = memgraph::query::v2::accessors::VertexAccessor;
using EdgeAccessor = memgraph::query::v2::accessors::EdgeAccessor;
using Path = memgraph::query::v2::accessors::Path;

// macro for the default implementation of LogicalOperator::Accept
// that accepts the visitor and visits it's input_ operator
#define ACCEPT_WITH_INPUT(class_name)                                    \
  bool class_name::Accept(HierarchicalLogicalOperatorVisitor &visitor) { \
    if (visitor.PreVisit(*this)) {                                       \
      input_->Accept(visitor);                                           \
    }                                                                    \
    return visitor.PostVisit(*this);                                     \
  }

#define WITHOUT_SINGLE_INPUT(class_name)                         \
  bool class_name::HasSingleInput() const { return false; }      \
  std::shared_ptr<LogicalOperator> class_name::input() const {   \
    LOG_FATAL("Operator " #class_name " has no single input!");  \
  }                                                              \
  void class_name::set_input(std::shared_ptr<LogicalOperator>) { \
    LOG_FATAL("Operator " #class_name " has no single input!");  \
  }

namespace EventCounter {
extern const Event OnceOperator;
extern const Event CreateNodeOperator;
extern const Event CreateExpandOperator;
extern const Event ScanAllOperator;
extern const Event ScanAllByLabelOperator;
extern const Event ScanAllByLabelPropertyRangeOperator;
extern const Event ScanAllByLabelPropertyValueOperator;
extern const Event ScanAllByLabelPropertyOperator;
extern const Event ScanByPrimaryKeyOperator;
extern const Event ExpandOperator;
extern const Event ExpandVariableOperator;
extern const Event ConstructNamedPathOperator;
extern const Event FilterOperator;
extern const Event ProduceOperator;
extern const Event DeleteOperator;
extern const Event SetPropertyOperator;
extern const Event SetPropertiesOperator;
extern const Event SetLabelsOperator;
extern const Event RemovePropertyOperator;
extern const Event RemoveLabelsOperator;
extern const Event EdgeUniquenessFilterOperator;
extern const Event AccumulateOperator;
extern const Event AggregateOperator;
extern const Event SkipOperator;
extern const Event LimitOperator;
extern const Event OrderByOperator;
extern const Event MergeOperator;
extern const Event OptionalOperator;
extern const Event UnwindOperator;
extern const Event DistinctOperator;
extern const Event UnionOperator;
extern const Event CartesianOperator;
extern const Event CallProcedureOperator;
extern const Event ForeachOperator;
}  // namespace EventCounter

namespace memgraph::query::v2::plan {

namespace {

// Custom equality function for a vector of typed values.
// Used in unordered_maps in Aggregate and Distinct operators.
struct TypedValueVectorEqual {
  template <class TAllocator>
  bool operator()(const std::vector<TypedValue, TAllocator> &left,
                  const std::vector<TypedValue, TAllocator> &right) const {
    MG_ASSERT(left.size() == right.size(),
              "TypedValueVector comparison should only be done over vectors "
              "of the same size");
    return std::equal(left.begin(), left.end(), right.begin(), TypedValue::BoolEqual{});
  }
};

// Returns boolean result of evaluating filter expression. Null is treated as
// false. Other non boolean values raise a QueryRuntimeException.
bool EvaluateFilter(ExpressionEvaluator &evaluator, Expression *filter) {
  TypedValue result = filter->Accept(evaluator);
  // Null is treated like false.
  if (result.IsNull()) return false;
  if (result.type() != TypedValue::Type::Bool)
    throw QueryRuntimeException("Filter expression must evaluate to bool or null, got {}.", result.type());
  return result.ValueBool();
}

template <typename T>
uint64_t ComputeProfilingKey(const T *obj) {
  static_assert(sizeof(T *) == sizeof(uint64_t));
  return reinterpret_cast<uint64_t>(obj);
}

}  // namespace

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define SCOPED_PROFILE_OP(name) \
  ScopedProfile profile { ComputeProfilingKey(this), name, &context }

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define SCOPED_CUSTOM_PROFILE(name) \
  ScopedCustomProfile custom_profile { name, context }

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define SCOPED_REQUEST_WAIT_PROFILE SCOPED_CUSTOM_PROFILE("request_wait")

class DistributedCreateNodeCursor : public Cursor {
 public:
  using InputOperator = std::shared_ptr<memgraph::query::v2::plan::LogicalOperator>;
  DistributedCreateNodeCursor(const InputOperator &op, utils::MemoryResource *mem, const NodeCreationInfo &node_info)
      : input_cursor_(op->MakeCursor(mem)), node_info_(node_info) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("CreateNode");
    if (input_cursor_->Pull(frame, context)) {
      auto &request_router = context.request_router;
      {
        SCOPED_REQUEST_WAIT_PROFILE;
        request_router->CreateVertices(NodeCreationInfoToRequest(context, frame));
      }
      PlaceNodeOnTheFrame(frame, context);
      return true;
    }

    return false;
  }

  bool PullMultiple(MultiFrame &multi_frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("CreateNodeMF");

    auto *request_router = context.request_router;
    if (!input_cursor_->PullMultiple(multi_frame, context)) {
      return false;
    }
    {
      SCOPED_REQUEST_WAIT_PROFILE;
      request_router->CreateVertices(NodeCreationInfoToRequests(context, multi_frame));
    }
    PlaceNodesOnTheMultiFrame(multi_frame, context);
    return true;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {}

  void PlaceNodeOnTheFrame(Frame &frame, ExecutionContext &context) {
    // TODO(kostasrim) Make this work with batching
    const auto primary_label = msgs::Label{.id = node_info_.labels[0]};
    msgs::Vertex v{.id = std::make_pair(primary_label, primary_keys_[0])};
    frame[node_info_.symbol] =
        TypedValue(query::v2::accessors::VertexAccessor(std::move(v), src_vertex_props_[0], context.request_router));
  }

  std::vector<msgs::NewVertex> NodeCreationInfoToRequest(ExecutionContext &context, Frame &frame) {
    primary_keys_.clear();
    std::vector<msgs::NewVertex> requests;
    msgs::PrimaryKey pk;
    msgs::NewVertex rqst;
    MG_ASSERT(!node_info_.labels.empty(), "Cannot determine primary label");
    const auto primary_label = node_info_.labels[0];
    // TODO(jbajic) Send also the properties that are not part of primary key
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, nullptr,
                                  storage::v3::View::NEW);
    if (const auto *node_info_properties = std::get_if<PropertiesMapList>(&node_info_.properties)) {
      for (const auto &[property, value_expression] : *node_info_properties) {
        TypedValue val = value_expression->Accept(evaluator);
        auto msgs_value = TypedValueToValue(val);
        if (context.request_router->IsPrimaryProperty(primary_label, property)) {
          rqst.primary_key.push_back(msgs_value);
          pk.push_back(std::move(msgs_value));
        } else {
          rqst.properties.emplace_back(property, std::move(msgs_value));
        }
      }
    } else {
      auto property_map = evaluator.Visit(*std::get<ParameterLookup *>(node_info_.properties)).ValueMap();
      for (const auto &[property, typed_value] : property_map) {
        auto property_str = std::string(property);
        auto property_id = context.request_router->NameToProperty(property_str);
        auto msgs_value = TypedValueToValue(typed_value);
        if (context.request_router->IsPrimaryProperty(primary_label, property_id)) {
          rqst.primary_key.push_back(msgs_value);
          pk.push_back(std::move(msgs_value));
        } else
          rqst.properties.emplace_back(property_id, std::move(msgs_value));
      }
    }

    // TODO(kostasrim) Copy non primary labels as well
    rqst.label_ids.push_back(msgs::Label{.id = primary_label});
    src_vertex_props_.push_back(rqst.properties);
    requests.push_back(std::move(rqst));

    primary_keys_.push_back(std::move(pk));
    return requests;
  }

  void PlaceNodesOnTheMultiFrame(MultiFrame &multi_frame, ExecutionContext &context) {
    auto multi_frame_modifier = multi_frame.GetValidFramesModifier();
    size_t i = 0;
    MG_ASSERT(std::distance(multi_frame_modifier.begin(), multi_frame_modifier.end()));
    for (auto &frame : multi_frame_modifier) {
      const auto primary_label = msgs::Label{.id = node_info_.labels[0]};
      msgs::Vertex v{.id = std::make_pair(primary_label, primary_keys_[i])};
      frame[node_info_.symbol] = TypedValue(
          query::v2::accessors::VertexAccessor(std::move(v), src_vertex_props_[i++], context.request_router));
    }
  }

  std::vector<msgs::NewVertex> NodeCreationInfoToRequests(ExecutionContext &context, MultiFrame &multi_frame) {
    primary_keys_.clear();
    std::vector<msgs::NewVertex> requests;
    auto multi_frame_modifier = multi_frame.GetValidFramesModifier();
    for (auto &frame : multi_frame_modifier) {
      msgs::PrimaryKey pk;
      msgs::NewVertex rqst;
      MG_ASSERT(!node_info_.labels.empty(), "Cannot determine primary label");
      const auto primary_label = node_info_.labels[0];
      MG_ASSERT(context.request_router->IsPrimaryLabel(primary_label), "First label has to be a primary label!");
      // TODO(jbajic) Send also the properties that are not part of primary key
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, nullptr,
                                    storage::v3::View::NEW);
      if (const auto *node_info_properties = std::get_if<PropertiesMapList>(&node_info_.properties)) {
        for (const auto &[property, value_expression] : *node_info_properties) {
          TypedValue val = value_expression->Accept(evaluator);
          auto msgs_value = TypedValueToValue(val);
          if (context.request_router->IsPrimaryProperty(primary_label, property)) {
            rqst.primary_key.push_back(msgs_value);
            pk.push_back(std::move(msgs_value));
          } else {
            rqst.properties.emplace_back(property, std::move(msgs_value));
          }
        }
      } else {
        auto property_map = evaluator.Visit(*std::get<ParameterLookup *>(node_info_.properties)).ValueMap();
        for (const auto &[property, typed_value] : property_map) {
          auto property_str = std::string(property);
          auto property_id = context.request_router->NameToProperty(property_str);
          auto msgs_value = TypedValueToValue(typed_value);
          if (context.request_router->IsPrimaryProperty(primary_label, property_id)) {
            rqst.primary_key.push_back(msgs_value);
            pk.push_back(std::move(msgs_value));
          } else
            rqst.properties.emplace_back(property_id, std::move(msgs_value));
        }
      }

      // TODO(kostasrim) Copy non primary labels as well
      rqst.label_ids.push_back(msgs::Label{.id = primary_label});
      src_vertex_props_.push_back(rqst.properties);
      requests.push_back(std::move(rqst));
      primary_keys_.push_back(std::move(pk));
    }

    return requests;
  }

 private:
  const UniqueCursorPtr input_cursor_;
  NodeCreationInfo node_info_;
  std::vector<std::vector<std::pair<storage::v3::PropertyId, msgs::Value>>> src_vertex_props_;
  std::vector<msgs::PrimaryKey> primary_keys_;
};

bool Once::OnceCursor::Pull(Frame &, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Once");

  if (!did_pull_) {
    did_pull_ = true;
    return true;
  }
  return false;
}

bool Once::OnceCursor::PullMultiple(MultiFrame &output_multi_frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("OnceMF");

  if (!did_pull_) {
    did_pull_ = true;
    if (pushed_down_multi_frame_.has_value()) {
      auto pushed_down_consumer = pushed_down_multi_frame_->GetValidFramesConsumer();
      auto output_populator = output_multi_frame.GetInvalidFramesPopulator();
      auto consumer_it = pushed_down_consumer.begin();
      auto populator_it = output_populator.begin();
      for (; consumer_it != pushed_down_consumer.end(); ++consumer_it, ++populator_it) {
        MG_ASSERT(populator_it != output_populator.end());
        *populator_it = std::move(*consumer_it);
      }
    } else {
      auto &first_frame = output_multi_frame.GetFirstFrame();
      first_frame.MakeValid();
    }
    return true;
  }
  return false;
}

void Once::OnceCursor::PushDown(const MultiFrame &multi_frame) { pushed_down_multi_frame_.emplace(multi_frame); }

UniqueCursorPtr Once::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::OnceOperator);

  return MakeUniqueCursorPtr<OnceCursor>(mem);
}

WITHOUT_SINGLE_INPUT(Once);

void Once::OnceCursor::Shutdown() {}

void Once::OnceCursor::Reset() { did_pull_ = false; }

CreateNode::CreateNode(const std::shared_ptr<LogicalOperator> &input, const NodeCreationInfo &node_info)
    : input_(input ? input : std::make_shared<Once>()), node_info_(node_info) {}

ACCEPT_WITH_INPUT(CreateNode)

UniqueCursorPtr CreateNode::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::CreateNodeOperator);

  return MakeUniqueCursorPtr<DistributedCreateNodeCursor>(mem, input_, mem, this->node_info_);
}

std::vector<Symbol> CreateNode::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(node_info_.symbol);
  return symbols;
}

CreateNode::CreateNodeCursor::CreateNodeCursor(const CreateNode &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool CreateNode::CreateNodeCursor::Pull(Frame & /*frame*/, ExecutionContext & /*context*/) { return false; }

void CreateNode::CreateNodeCursor::Shutdown() { input_cursor_->Shutdown(); }

void CreateNode::CreateNodeCursor::Reset() { input_cursor_->Reset(); }

CreateExpand::CreateExpand(const NodeCreationInfo &node_info, const EdgeCreationInfo &edge_info,
                           const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol, bool existing_node)
    : node_info_(node_info),
      edge_info_(edge_info),
      input_(input ? input : std::make_shared<Once>()),
      input_symbol_(input_symbol),
      existing_node_(existing_node) {}

ACCEPT_WITH_INPUT(CreateExpand)

class DistributedCreateExpandCursor;

UniqueCursorPtr CreateExpand::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::CreateNodeOperator);

  return MakeUniqueCursorPtr<DistributedCreateExpandCursor>(mem, input_, mem, *this);
}

std::vector<Symbol> CreateExpand::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(node_info_.symbol);
  symbols.emplace_back(edge_info_.symbol);
  return symbols;
}

CreateExpand::CreateExpandCursor::CreateExpandCursor(const CreateExpand &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool CreateExpand::CreateExpandCursor::Pull(Frame & /*frame*/, ExecutionContext & /*context*/) { return false; }

void CreateExpand::CreateExpandCursor::Shutdown() { input_cursor_->Shutdown(); }

void CreateExpand::CreateExpandCursor::Reset() { input_cursor_->Reset(); }

template <class TVerticesFun>
class ScanAllCursor : public Cursor {
 public:
  explicit ScanAllCursor(Symbol output_symbol, UniqueCursorPtr input_cursor, TVerticesFun get_vertices,
                         const char *op_name)
      : output_symbol_(output_symbol),
        input_cursor_(std::move(input_cursor)),
        get_vertices_(std::move(get_vertices)),
        op_name_(op_name) {}

  bool Pull(Frame & /*frame*/, ExecutionContext & /*context*/) override { return false; }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    vertices_ = std::nullopt;
    vertices_it_ = std::nullopt;
  }

 private:
  const Symbol output_symbol_;
  const UniqueCursorPtr input_cursor_;
  TVerticesFun get_vertices_;
  std::optional<typename std::result_of<TVerticesFun(Frame &, ExecutionContext &)>::type::value_type> vertices_;
  std::optional<decltype(vertices_.value().begin())> vertices_it_;
  const char *op_name_;
  std::vector<msgs::ScanVerticesResponse> current_batch;
};

class DistributedScanAllAndFilterCursor : public Cursor {
 public:
  explicit DistributedScanAllAndFilterCursor(
      Symbol output_symbol, UniqueCursorPtr input_cursor, const char *op_name,
      std::optional<storage::v3::LabelId> label,
      std::optional<std::pair<storage::v3::PropertyId, Expression *>> property_expression_pair,
      std::optional<std::vector<Expression *>> filter_expressions)
      : output_symbol_(output_symbol),
        input_cursor_(std::move(input_cursor)),
        op_name_(op_name),
        label_(label),
        property_expression_pair_(property_expression_pair),
        filter_expressions_(filter_expressions) {
    ResetExecutionState();
  }

  using VertexAccessor = accessors::VertexAccessor;

  bool MakeRequest(ExecutionContext &context) {
    {
      SCOPED_REQUEST_WAIT_PROFILE;
      std::optional<std::string> request_label = std::nullopt;
      if (label_.has_value()) {
        request_label = context.request_router->LabelToName(*label_);
      }
      current_batch_ = context.request_router->ScanVertices(request_label);
    }
    current_vertex_it_ = current_batch_.begin();
    return !current_batch_.empty();
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP(op_name_);

    while (true) {
      if (MustAbort(context)) {
        throw HintedAbortError();
      }

      if (current_vertex_it_ == current_batch_.end()) {
        ResetExecutionState();
        if (!input_cursor_->Pull(frame, context) || !MakeRequest(context)) {
          return false;
        }
      }

      frame[output_symbol_] = TypedValue(std::move(*current_vertex_it_));
      ++current_vertex_it_;
      return true;
    }
  }

  bool PullMultiple(MultiFrame &output_multi_frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP(op_name_);

    if (!own_multi_frame_.has_value()) {
      own_multi_frame_.emplace(MultiFrame(output_multi_frame.GetFirstFrame().Elems().size(),
                                          FLAGS_default_multi_frame_size, output_multi_frame.GetMemoryResource()));
      own_frames_consumer_.emplace(own_multi_frame_->GetValidFramesConsumer());
      own_frames_it_ = own_frames_consumer_->begin();
    }

    auto output_frames_populator = output_multi_frame.GetInvalidFramesPopulator();
    auto populated_any = false;

    while (true) {
      switch (state_) {
        case State::PullInput: {
          if (!input_cursor_->PullMultiple(*own_multi_frame_, context)) {
            state_ = State::Exhausted;
            return populated_any;
          }
          own_frames_consumer_.emplace(own_multi_frame_->GetValidFramesConsumer());
          own_frames_it_ = own_frames_consumer_->begin();
          state_ = State::FetchVertices;
          break;
        }
        case State::FetchVertices: {
          if (own_frames_it_ == own_frames_consumer_->end()) {
            state_ = State::PullInput;
            continue;
          }
          if (!filter_expressions_->empty() || property_expression_pair_.has_value() || current_batch_.empty()) {
            MakeRequest(context);
          } else {
            // We can reuse the vertices as they don't depend on any value from the frames
            current_vertex_it_ = current_batch_.begin();
          }
          state_ = State::PopulateOutput;
          break;
        }
        case State::PopulateOutput: {
          if (!output_multi_frame.HasInvalidFrame()) {
            return populated_any;
          }
          if (current_vertex_it_ == current_batch_.end()) {
            own_frames_it_->MakeInvalid();
            ++own_frames_it_;
            state_ = State::FetchVertices;
            continue;
          }

          for (auto output_frame_it = output_frames_populator.begin();
               output_frame_it != output_frames_populator.end() && current_vertex_it_ != current_batch_.end();
               ++output_frame_it) {
            auto &output_frame = *output_frame_it;
            output_frame = *own_frames_it_;
            output_frame[output_symbol_] = TypedValue(*current_vertex_it_);
            current_vertex_it_++;
            populated_any = true;
          }
          break;
        }
        case State::Exhausted: {
          return populated_any;
        }
      }
    }
    return populated_any;
  };

  void PushDown(const MultiFrame &multi_frame) override { input_cursor_->PushDown(multi_frame); }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void ResetExecutionState() {
    current_batch_.clear();
    current_vertex_it_ = current_batch_.end();
  }

  void Reset() override {
    input_cursor_->Reset();
    ResetExecutionState();
  }

 private:
  enum class State { PullInput, FetchVertices, PopulateOutput, Exhausted };

  State state_{State::PullInput};
  const Symbol output_symbol_;
  const UniqueCursorPtr input_cursor_;
  const char *op_name_;
  std::vector<VertexAccessor> current_batch_;
  std::vector<VertexAccessor>::iterator current_vertex_it_{current_batch_.begin()};
  std::optional<storage::v3::LabelId> label_;
  std::optional<std::pair<storage::v3::PropertyId, Expression *>> property_expression_pair_;
  std::optional<std::vector<Expression *>> filter_expressions_;
  std::optional<MultiFrame> own_multi_frame_;
  std::optional<ValidFramesConsumer> own_frames_consumer_;
  ValidFramesConsumer::Iterator own_frames_it_;
};

class DistributedScanByPrimaryKeyCursor : public Cursor {
 public:
  explicit DistributedScanByPrimaryKeyCursor(Symbol output_symbol, UniqueCursorPtr input_cursor, const char *op_name,
                                             storage::v3::LabelId label,
                                             std::optional<std::vector<Expression *>> filter_expressions,
                                             std::vector<Expression *> primary_key)
      : output_symbol_(output_symbol),
        input_cursor_(std::move(input_cursor)),
        op_name_(op_name),
        label_(label),
        filter_expressions_(filter_expressions),
        primary_key_(primary_key) {}

  using VertexAccessor = accessors::VertexAccessor;

  std::optional<VertexAccessor> MakeRequestSingleFrame(Frame &frame, RequestRouterInterface &request_router,
                                                       ExecutionContext &context) {
    // Evaluate the expressions that hold the PrimaryKey.
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                  storage::v3::View::NEW);

    std::vector<msgs::Value> pk;
    for (auto *primary_property : primary_key_) {
      pk.push_back(TypedValueToValue(primary_property->Accept(evaluator)));
    }

    msgs::Label label = {.id = msgs::LabelId::FromUint(label_.AsUint())};

    msgs::GetPropertiesRequest req = {.vertex_ids = {std::make_pair(label, pk)}};
    auto get_prop_result = std::invoke([&context, &request_router, &req]() mutable {
      SCOPED_REQUEST_WAIT_PROFILE;
      return request_router.GetProperties(req);
    });
    MG_ASSERT(get_prop_result.size() <= 1);

    if (get_prop_result.empty()) {
      return std::nullopt;
    }
    auto properties = get_prop_result[0].props;
    // TODO (gvolfing) figure out labels when relevant.
    msgs::Vertex vertex = {.id = get_prop_result[0].vertex, .labels = {}};

    return VertexAccessor(vertex, properties, &request_router);
  }

  void MakeRequestMultiFrame(MultiFrame &multi_frame, RequestRouterInterface &request_router,
                             ExecutionContext &context) {
    msgs::GetPropertiesRequest req;
    const msgs::Label label = {.id = msgs::LabelId::FromUint(label_.AsUint())};

    std::unordered_set<msgs::VertexId> used_vertex_ids;

    for (auto &frame : multi_frame.GetValidFramesModifier()) {
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                    storage::v3::View::NEW);

      std::vector<msgs::Value> pk;
      for (auto *primary_property : primary_key_) {
        pk.push_back(TypedValueToValue(primary_property->Accept(evaluator)));
      }

      auto vertex_id = std::make_pair(label, std::move(pk));
      auto [it, inserted] = used_vertex_ids.emplace(std::move(vertex_id));
      if (inserted) {
        req.vertex_ids.emplace_back(*it);
      }
    }

    auto get_prop_result = std::invoke([&context, &request_router, &req]() mutable {
      SCOPED_REQUEST_WAIT_PROFILE;
      return request_router.GetProperties(req);
    });

    for (auto &result : get_prop_result) {
      // TODO (gvolfing) figure out labels when relevant.
      msgs::Vertex vertex = {.id = result.vertex, .labels = {}};

      id_to_accessor_mapping_.emplace(result.vertex,
                                      VertexAccessor(std::move(vertex), std::move(result.props), &request_router));
    }
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP(op_name_);

    if (MustAbort(context)) {
      throw HintedAbortError();
    }

    while (input_cursor_->Pull(frame, context)) {
      auto &request_router = *context.request_router;
      auto vertex = MakeRequestSingleFrame(frame, request_router, context);
      if (vertex) {
        frame[output_symbol_] = TypedValue(std::move(*vertex));
        return true;
      }
    }
    return false;
  }

  void EnsureOwnMultiFrameIsGood(MultiFrame &output_multi_frame) {
    if (!own_multi_frame_.has_value()) {
      own_multi_frame_.emplace(MultiFrame(output_multi_frame.GetFirstFrame().Elems().size(),
                                          FLAGS_default_multi_frame_size, output_multi_frame.GetMemoryResource()));
      own_frames_consumer_.emplace(own_multi_frame_->GetValidFramesConsumer());
      own_frames_it_ = own_frames_consumer_->begin();
    }
    MG_ASSERT(output_multi_frame.GetFirstFrame().Elems().size() == own_multi_frame_->GetFirstFrame().Elems().size());
  }

  bool PullMultiple(MultiFrame &output_multi_frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP(op_name_);
    EnsureOwnMultiFrameIsGood(output_multi_frame);

    auto output_frames_populator = output_multi_frame.GetInvalidFramesPopulator();
    auto populated_any = false;

    while (true) {
      switch (state_) {
        case State::PullInput: {
          id_to_accessor_mapping_.clear();
          if (!input_cursor_->PullMultiple(*own_multi_frame_, context)) {
            state_ = State::Exhausted;
            return populated_any;
          }
          own_frames_consumer_.emplace(own_multi_frame_->GetValidFramesConsumer());
          own_frames_it_ = own_frames_consumer_->begin();

          if (own_frames_it_ == own_frames_consumer_->end()) {
            continue;
          }

          MakeRequestMultiFrame(*own_multi_frame_, *context.request_router, context);

          state_ = State::PopulateOutput;
          break;
        }
        case State::PopulateOutput: {
          if (!output_multi_frame.HasInvalidFrame()) {
            if (own_frames_it_ == own_frames_consumer_->end()) {
              id_to_accessor_mapping_.clear();
            }
            return populated_any;
          }

          if (own_frames_it_ == own_frames_consumer_->end()) {
            state_ = State::PullInput;
            continue;
          }

          for (auto output_frame_it = output_frames_populator.begin();
               output_frame_it != output_frames_populator.end() && own_frames_it_ != own_frames_consumer_->end();
               ++own_frames_it_) {
            auto &output_frame = *output_frame_it;

            ExpressionEvaluator evaluator(&*own_frames_it_, context.symbol_table, context.evaluation_context,
                                          context.request_router, storage::v3::View::NEW);

            std::vector<msgs::Value> pk;
            for (auto *primary_property : primary_key_) {
              pk.push_back(TypedValueToValue(primary_property->Accept(evaluator)));
            }

            const msgs::Label label = {.id = msgs::LabelId::FromUint(label_.AsUint())};
            auto vertex_id = std::make_pair(label, std::move(pk));

            if (const auto it = id_to_accessor_mapping_.find(vertex_id); it != id_to_accessor_mapping_.end()) {
              output_frame = *own_frames_it_;
              output_frame[output_symbol_] = TypedValue(it->second);
              populated_any = true;
              ++output_frame_it;
            }
            own_frames_it_->MakeInvalid();
          }
          break;
        }
        case State::Exhausted: {
          return populated_any;
        }
      }
    }
    return populated_any;
  };

  void Reset() override { input_cursor_->Reset(); }

  void Shutdown() override { input_cursor_->Shutdown(); }

 private:
  enum class State { PullInput, PopulateOutput, Exhausted };

  State state_{State::PullInput};
  const Symbol output_symbol_;
  const UniqueCursorPtr input_cursor_;
  const char *op_name_;
  storage::v3::LabelId label_;
  std::optional<std::vector<Expression *>> filter_expressions_;
  std::vector<Expression *> primary_key_;
  std::optional<MultiFrame> own_multi_frame_;
  std::optional<ValidFramesConsumer> own_frames_consumer_;
  ValidFramesConsumer::Iterator own_frames_it_;
  std::unordered_map<msgs::VertexId, VertexAccessor> id_to_accessor_mapping_;
};

ScanAll::ScanAll(const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol, storage::v3::View view)
    : input_(input ? input : std::make_shared<Once>()), output_symbol_(output_symbol), view_(view) {}

ACCEPT_WITH_INPUT(ScanAll)

UniqueCursorPtr ScanAll::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllOperator);

  return MakeUniqueCursorPtr<DistributedScanAllAndFilterCursor>(
      mem, output_symbol_, input_->MakeCursor(mem), "ScanAll", std::nullopt /*label*/,
      std::nullopt /*property_expression_pair*/, std::nullopt /*filter_expressions*/);
}

std::vector<Symbol> ScanAll::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(output_symbol_);
  return symbols;
}

ScanAllByLabel::ScanAllByLabel(const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol,
                               storage::v3::LabelId label, storage::v3::View view)
    : ScanAll(input, output_symbol, view), label_(label) {}

ACCEPT_WITH_INPUT(ScanAllByLabel)

UniqueCursorPtr ScanAllByLabel::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByLabelOperator);

  return MakeUniqueCursorPtr<DistributedScanAllAndFilterCursor>(
      mem, output_symbol_, input_->MakeCursor(mem), "ScanAllByLabel", label_, std::nullopt /*property_expression_pair*/,
      std::nullopt /*filter_expressions*/);
}

// TODO(buda): Implement ScanAllByLabelProperty operator to iterate over
// vertices that have the label and some value for the given property.

ScanAllByLabelPropertyRange::ScanAllByLabelPropertyRange(const std::shared_ptr<LogicalOperator> &input,
                                                         Symbol output_symbol, storage::v3::LabelId label,
                                                         storage::v3::PropertyId property,
                                                         const std::string &property_name,
                                                         std::optional<Bound> lower_bound,
                                                         std::optional<Bound> upper_bound, storage::v3::View view)
    : ScanAll(input, output_symbol, view),
      label_(label),
      property_(property),
      property_name_(property_name),
      lower_bound_(lower_bound),
      upper_bound_(upper_bound) {
  MG_ASSERT(lower_bound_ || upper_bound_, "Only one bound can be left out");
}

ACCEPT_WITH_INPUT(ScanAllByLabelPropertyRange)

UniqueCursorPtr ScanAllByLabelPropertyRange::MakeCursor(utils::MemoryResource * /*mem*/) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByLabelPropertyRangeOperator);

  throw QueryRuntimeException("ScanAllByLabelPropertyRange is not supported");
}

ScanAllByLabelPropertyValue::ScanAllByLabelPropertyValue(const std::shared_ptr<LogicalOperator> &input,
                                                         Symbol output_symbol, storage::v3::LabelId label,
                                                         storage::v3::PropertyId property,
                                                         const std::string &property_name, Expression *expression,
                                                         storage::v3::View view)
    : ScanAll(input, output_symbol, view),
      label_(label),
      property_(property),
      property_name_(property_name),
      expression_(expression) {
  DMG_ASSERT(expression, "Expression is not optional.");
}

ACCEPT_WITH_INPUT(ScanAllByLabelPropertyValue)

UniqueCursorPtr ScanAllByLabelPropertyValue::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByLabelPropertyValueOperator);

  return MakeUniqueCursorPtr<DistributedScanAllAndFilterCursor>(
      mem, output_symbol_, input_->MakeCursor(mem), "ScanAllByLabelPropertyValue", label_,
      std::make_pair(property_, expression_), std::nullopt /*filter_expressions*/);
}

ScanAllByLabelProperty::ScanAllByLabelProperty(const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol,
                                               storage::v3::LabelId label, storage::v3::PropertyId property,
                                               const std::string &property_name, storage::v3::View view)
    : ScanAll(input, output_symbol, view), label_(label), property_(property), property_name_(property_name) {}

ACCEPT_WITH_INPUT(ScanAllByLabelProperty)

UniqueCursorPtr ScanAllByLabelProperty::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByLabelPropertyOperator);
  throw QueryRuntimeException("ScanAllByLabelProperty is not supported");
}

ScanByPrimaryKey::ScanByPrimaryKey(const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol,
                                   storage::v3::LabelId label, std::vector<query::v2::Expression *> primary_key,
                                   storage::v3::View view)
    : ScanAll(input, output_symbol, view), label_(label), primary_key_(primary_key) {
  MG_ASSERT(primary_key.front());
}

ACCEPT_WITH_INPUT(ScanByPrimaryKey)

UniqueCursorPtr ScanByPrimaryKey::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanByPrimaryKeyOperator);

  return MakeUniqueCursorPtr<DistributedScanByPrimaryKeyCursor>(mem, output_symbol_, input_->MakeCursor(mem),
                                                                "ScanByPrimaryKey", label_,
                                                                std::nullopt /*filter_expressions*/, primary_key_);
}

Expand::Expand(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol, Symbol node_symbol,
               Symbol edge_symbol, EdgeAtom::Direction direction,
               const std::vector<storage::v3::EdgeTypeId> &edge_types, bool existing_node, storage::v3::View view)
    : input_(input ? input : std::make_shared<Once>()),
      input_symbol_(input_symbol),
      common_{node_symbol, edge_symbol, direction, edge_types, existing_node},
      view_(view) {}

ACCEPT_WITH_INPUT(Expand)

class DistributedExpandCursor;

UniqueCursorPtr Expand::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ExpandOperator);

  return MakeUniqueCursorPtr<DistributedExpandCursor>(mem, *this, mem);
}

std::vector<Symbol> Expand::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(common_.node_symbol);
  symbols.emplace_back(common_.edge_symbol);
  return symbols;
}

Expand::ExpandCursor::ExpandCursor(const Expand &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool Expand::ExpandCursor::Pull(Frame & /*frame*/, ExecutionContext & /*context*/) { return false; }

void Expand::ExpandCursor::Shutdown() { input_cursor_->Shutdown(); }

void Expand::ExpandCursor::Reset() {
  input_cursor_->Reset();
  in_edges_ = std::nullopt;
  in_edges_it_ = std::nullopt;
  out_edges_ = std::nullopt;
  out_edges_it_ = std::nullopt;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool Expand::ExpandCursor::InitEdges(Frame & /*frame*/, ExecutionContext & /*context*/) { return true; }

ExpandVariable::ExpandVariable(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol, Symbol node_symbol,
                               Symbol edge_symbol, EdgeAtom::Type type, EdgeAtom::Direction direction,
                               const std::vector<storage::v3::EdgeTypeId> &edge_types, bool is_reverse,
                               Expression *lower_bound, Expression *upper_bound, bool existing_node,
                               ExpansionLambda filter_lambda, std::optional<ExpansionLambda> weight_lambda,
                               std::optional<Symbol> total_weight)
    : input_(input ? input : std::make_shared<Once>()),
      input_symbol_(input_symbol),
      common_{node_symbol, edge_symbol, direction, edge_types, existing_node},
      type_(type),
      is_reverse_(is_reverse),
      lower_bound_(lower_bound),
      upper_bound_(upper_bound),
      filter_lambda_(filter_lambda),
      weight_lambda_(weight_lambda),
      total_weight_(total_weight) {
  DMG_ASSERT(type_ == EdgeAtom::Type::DEPTH_FIRST || type_ == EdgeAtom::Type::BREADTH_FIRST ||
                 type_ == EdgeAtom::Type::WEIGHTED_SHORTEST_PATH,
             "ExpandVariable can only be used with breadth first, depth first or "
             "weighted shortest path type");
  DMG_ASSERT(!(type_ == EdgeAtom::Type::BREADTH_FIRST && is_reverse), "Breadth first expansion can't be reversed");
}

ACCEPT_WITH_INPUT(ExpandVariable)

std::vector<Symbol> ExpandVariable::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(common_.node_symbol);
  symbols.emplace_back(common_.edge_symbol);
  return symbols;
}

UniqueCursorPtr ExpandVariable::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ExpandVariableOperator);

  throw QueryRuntimeException("ExpandVariable is not supported");
}

class ConstructNamedPathCursor : public Cursor {
 public:
  ConstructNamedPathCursor(const ConstructNamedPath &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_.input()->MakeCursor(mem)) {}

  bool Pull(Frame & /*frame*/, ExecutionContext & /*context*/) override { return false; }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override { input_cursor_->Reset(); }

 private:
  const ConstructNamedPath self_;
  const UniqueCursorPtr input_cursor_;
};

ACCEPT_WITH_INPUT(ConstructNamedPath)

UniqueCursorPtr ConstructNamedPath::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ConstructNamedPathOperator);

  return MakeUniqueCursorPtr<ConstructNamedPathCursor>(mem, *this, mem);
}

std::vector<Symbol> ConstructNamedPath::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(path_symbol_);
  return symbols;
}

Filter::Filter(const std::shared_ptr<LogicalOperator> &input, Expression *expression)
    : input_(input ? input : std::make_shared<Once>()), expression_(expression) {}

ACCEPT_WITH_INPUT(Filter)

UniqueCursorPtr Filter::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::FilterOperator);

  return MakeUniqueCursorPtr<FilterCursor>(mem, *this, mem);
}

std::vector<Symbol> Filter::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Filter::FilterCursor::FilterCursor(const Filter &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Filter::FilterCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Filter");

  // Like all filters, newly set values should not affect filtering of old
  // nodes and edges.
  ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                storage::v3::View::OLD);
  while (input_cursor_->Pull(frame, context)) {
    if (EvaluateFilter(evaluator, self_.expression_)) return true;
  }
  return false;
}

bool Filter::FilterCursor::PullMultiple(MultiFrame &multi_frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Filter");
  auto populated_any = false;

  while (multi_frame.HasInvalidFrame()) {
    if (!input_cursor_->PullMultiple(multi_frame, context)) {
      return populated_any;
    }
    for (auto &frame : multi_frame.GetValidFramesConsumer()) {
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                    storage::v3::View::OLD);
      if (!EvaluateFilter(evaluator, self_.expression_)) {
        frame.MakeInvalid();
      } else {
        populated_any = true;
      }
    }
  }
  return populated_any;
}

void Filter::FilterCursor::Shutdown() { input_cursor_->Shutdown(); }

void Filter::FilterCursor::Reset() { input_cursor_->Reset(); }

Produce::Produce(const std::shared_ptr<LogicalOperator> &input, const std::vector<NamedExpression *> &named_expressions)
    : input_(input ? input : std::make_shared<Once>()), named_expressions_(named_expressions) {}

ACCEPT_WITH_INPUT(Produce)

UniqueCursorPtr Produce::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ProduceOperator);

  return MakeUniqueCursorPtr<ProduceCursor>(mem, *this, mem);
}

std::vector<Symbol> Produce::OutputSymbols(const SymbolTable &symbol_table) const {
  std::vector<Symbol> symbols;
  for (const auto &named_expr : named_expressions_) {
    symbols.emplace_back(symbol_table.at(*named_expr));
  }
  return symbols;
}

std::vector<Symbol> Produce::ModifiedSymbols(const SymbolTable &table) const { return OutputSymbols(table); }

Produce::ProduceCursor::ProduceCursor(const Produce &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Produce::ProduceCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Produce");

  if (input_cursor_->Pull(frame, context)) {
    // Produce should always yield the latest results.
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                  storage::v3::View::NEW);
    for (auto *named_expr : self_.named_expressions_) named_expr->Accept(evaluator);

    return true;
  }
  return false;
}

bool Produce::ProduceCursor::PullMultiple(MultiFrame &multi_frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("ProduceMF");

  if (!input_cursor_->PullMultiple(multi_frame, context)) {
    return false;
  }

  auto iterator_for_valid_frame_only = multi_frame.GetValidFramesModifier();

  for (auto &frame : iterator_for_valid_frame_only) {
    // Produce should always yield the latest results.
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                  storage::v3::View::NEW);

    for (auto *named_expr : self_.named_expressions_) {
      named_expr->Accept(evaluator);
    }
  }

  return true;
}

void Produce::ProduceCursor::Shutdown() { input_cursor_->Shutdown(); }

void Produce::ProduceCursor::Reset() { input_cursor_->Reset(); }

Delete::Delete(const std::shared_ptr<LogicalOperator> &input_, const std::vector<Expression *> &expressions,
               bool detach_)
    : input_(input_), expressions_(expressions), detach_(detach_) {}

ACCEPT_WITH_INPUT(Delete)

UniqueCursorPtr Delete::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::DeleteOperator);

  return MakeUniqueCursorPtr<DeleteCursor>(mem, *this, mem);
}

std::vector<Symbol> Delete::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Delete::DeleteCursor::DeleteCursor(const Delete &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Delete::DeleteCursor::Pull(Frame & /*frame*/, ExecutionContext & /*context*/) { return false; }

bool Delete::DeleteCursor::PullMultiple(MultiFrame & /*multi_frame*/, ExecutionContext & /*context*/) { return false; }

void Delete::DeleteCursor::Shutdown() { input_cursor_->Shutdown(); }

void Delete::DeleteCursor::Reset() { input_cursor_->Reset(); }

SetProperty::SetProperty(const std::shared_ptr<LogicalOperator> &input, storage::v3::PropertyId property,
                         PropertyLookup *lhs, Expression *rhs)
    : input_(input), property_(property), lhs_(lhs), rhs_(rhs) {}

ACCEPT_WITH_INPUT(SetProperty)

UniqueCursorPtr SetProperty::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::SetPropertyOperator);

  return MakeUniqueCursorPtr<SetPropertyCursor>(mem, *this, mem);
}

std::vector<Symbol> SetProperty::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

SetProperty::SetPropertyCursor::SetPropertyCursor(const SetProperty &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool SetProperty::SetPropertyCursor::Pull(Frame & /*frame*/, ExecutionContext & /*context*/) { return false; }

void SetProperty::SetPropertyCursor::Shutdown() { input_cursor_->Shutdown(); }

void SetProperty::SetPropertyCursor::Reset() { input_cursor_->Reset(); }

SetProperties::SetProperties(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol, Expression *rhs, Op op)
    : input_(input), input_symbol_(input_symbol), rhs_(rhs), op_(op) {}

ACCEPT_WITH_INPUT(SetProperties)

UniqueCursorPtr SetProperties::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::SetPropertiesOperator);

  return MakeUniqueCursorPtr<SetPropertiesCursor>(mem, *this, mem);
}

std::vector<Symbol> SetProperties::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

SetProperties::SetPropertiesCursor::SetPropertiesCursor(const SetProperties &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool SetProperties::SetPropertiesCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("SetProperties");
  return false;
}

void SetProperties::SetPropertiesCursor::Shutdown() { input_cursor_->Shutdown(); }

void SetProperties::SetPropertiesCursor::Reset() { input_cursor_->Reset(); }

SetLabels::SetLabels(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol,
                     const std::vector<storage::v3::LabelId> &labels)
    : input_(input), input_symbol_(input_symbol), labels_(labels) {}

ACCEPT_WITH_INPUT(SetLabels)

UniqueCursorPtr SetLabels::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::SetLabelsOperator);

  return MakeUniqueCursorPtr<SetLabelsCursor>(mem, *this, mem);
}

std::vector<Symbol> SetLabels::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

SetLabels::SetLabelsCursor::SetLabelsCursor(const SetLabels &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool SetLabels::SetLabelsCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("SetLabels");
  return false;
}

void SetLabels::SetLabelsCursor::Shutdown() { input_cursor_->Shutdown(); }

void SetLabels::SetLabelsCursor::Reset() { input_cursor_->Reset(); }

RemoveProperty::RemoveProperty(const std::shared_ptr<LogicalOperator> &input, storage::v3::PropertyId property,
                               PropertyLookup *lhs)
    : input_(input), property_(property), lhs_(lhs) {}

ACCEPT_WITH_INPUT(RemoveProperty)

UniqueCursorPtr RemoveProperty::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::RemovePropertyOperator);

  return MakeUniqueCursorPtr<RemovePropertyCursor>(mem, *this, mem);
}

std::vector<Symbol> RemoveProperty::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

RemoveProperty::RemovePropertyCursor::RemovePropertyCursor(const RemoveProperty &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool RemoveProperty::RemovePropertyCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("RemoveProperty");
  return false;
}

void RemoveProperty::RemovePropertyCursor::Shutdown() { input_cursor_->Shutdown(); }

void RemoveProperty::RemovePropertyCursor::Reset() { input_cursor_->Reset(); }

RemoveLabels::RemoveLabels(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol,
                           const std::vector<storage::v3::LabelId> &labels)
    : input_(input), input_symbol_(input_symbol), labels_(labels) {}

ACCEPT_WITH_INPUT(RemoveLabels)

UniqueCursorPtr RemoveLabels::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::RemoveLabelsOperator);

  return MakeUniqueCursorPtr<RemoveLabelsCursor>(mem, *this, mem);
}

std::vector<Symbol> RemoveLabels::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

RemoveLabels::RemoveLabelsCursor::RemoveLabelsCursor(const RemoveLabels &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool RemoveLabels::RemoveLabelsCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("RemoveLabels");
  return false;
}

void RemoveLabels::RemoveLabelsCursor::Shutdown() { input_cursor_->Shutdown(); }

void RemoveLabels::RemoveLabelsCursor::Reset() { input_cursor_->Reset(); }

EdgeUniquenessFilter::EdgeUniquenessFilter(const std::shared_ptr<LogicalOperator> &input, Symbol expand_symbol,
                                           const std::vector<Symbol> &previous_symbols)
    : input_(input), expand_symbol_(expand_symbol), previous_symbols_(previous_symbols) {}

ACCEPT_WITH_INPUT(EdgeUniquenessFilter)

UniqueCursorPtr EdgeUniquenessFilter::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::EdgeUniquenessFilterOperator);

  return MakeUniqueCursorPtr<EdgeUniquenessFilterCursor>(mem, *this, mem);
}

std::vector<Symbol> EdgeUniquenessFilter::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

EdgeUniquenessFilter::EdgeUniquenessFilterCursor::EdgeUniquenessFilterCursor(const EdgeUniquenessFilter &self,
                                                                             utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

namespace {
/**
 * Returns true if:
 *    - a and b are either edge or edge-list values, and there
 *    is at least one matching edge in the two values
 */
bool ContainsSameEdge(const TypedValue &a, const TypedValue &b) {
  auto compare_to_list = [](const TypedValue &list, const TypedValue &other) {
    for (const TypedValue &list_elem : list.ValueList())
      if (ContainsSameEdge(list_elem, other)) return true;
    return false;
  };

  if (a.type() == TypedValue::Type::List) return compare_to_list(a, b);
  if (b.type() == TypedValue::Type::List) return compare_to_list(b, a);

  return a.ValueEdge() == b.ValueEdge();
}

bool IsExpansionOk(Frame &frame, const Symbol &expand_symbol, const std::vector<Symbol> &previous_symbols) {
  // This shouldn't raise a TypedValueException, because the planner
  // makes sure these are all of the expected type. In case they are not
  // an error should be raised long before this code is executed.
  return std::ranges::all_of(previous_symbols,
                             [&frame, &expand_value = frame[expand_symbol]](const auto &previous_symbol) {
                               const auto &previous_value = frame[previous_symbol];
                               return !ContainsSameEdge(previous_value, expand_value);
                             });
}

}  // namespace

bool EdgeUniquenessFilter::EdgeUniquenessFilterCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("EdgeUniquenessFilter");
  while (input_cursor_->Pull(frame, context))
    if (IsExpansionOk(frame, self_.expand_symbol_, self_.previous_symbols_)) return true;
  return false;
}

bool EdgeUniquenessFilter::EdgeUniquenessFilterCursor::PullMultiple(MultiFrame &output_multi_frame,
                                                                    ExecutionContext &context) {
  SCOPED_PROFILE_OP("EdgeUniquenessFilterMF");
  auto populated_any = false;

  while (output_multi_frame.HasInvalidFrame()) {
    if (!input_cursor_->PullMultiple(output_multi_frame, context)) {
      return populated_any;
    }
    for (auto &frame : output_multi_frame.GetValidFramesConsumer()) {
      if (IsExpansionOk(frame, self_.expand_symbol_, self_.previous_symbols_)) {
        populated_any = true;
      } else {
        frame.MakeInvalid();
      }
    }
  }
  return populated_any;
}

void EdgeUniquenessFilter::EdgeUniquenessFilterCursor::Shutdown() { input_cursor_->Shutdown(); }

void EdgeUniquenessFilter::EdgeUniquenessFilterCursor::Reset() { input_cursor_->Reset(); }

Accumulate::Accumulate(const std::shared_ptr<LogicalOperator> &input, const std::vector<Symbol> &symbols,
                       bool advance_command)
    : input_(input), symbols_(symbols), advance_command_(advance_command) {}

ACCEPT_WITH_INPUT(Accumulate)

std::vector<Symbol> Accumulate::ModifiedSymbols(const SymbolTable &) const { return symbols_; }

UniqueCursorPtr Accumulate::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::AccumulateOperator);
  throw QueryRuntimeException("Accumulate is not supported");
}

Aggregate::Aggregate(const std::shared_ptr<LogicalOperator> &input, const std::vector<Aggregate::Element> &aggregations,
                     const std::vector<Expression *> &group_by, const std::vector<Symbol> &remember)
    : input_(input ? input : std::make_shared<Once>()),
      aggregations_(aggregations),
      group_by_(group_by),
      remember_(remember) {}

ACCEPT_WITH_INPUT(Aggregate)

std::vector<Symbol> Aggregate::ModifiedSymbols(const SymbolTable &) const {
  auto symbols = remember_;
  for (const auto &elem : aggregations_) symbols.push_back(elem.output_sym);
  return symbols;
}

namespace {
/** Returns the default TypedValue for an Aggregation element.
 * This value is valid both for returning when where are no inputs
 * to the aggregation op, and for initializing an aggregation result
 * when there are */
TypedValue DefaultAggregationOpValue(const Aggregate::Element &element, utils::MemoryResource *memory) {
  switch (element.op) {
    case Aggregation::Op::COUNT:
      return TypedValue(0, memory);
    case Aggregation::Op::SUM:
    case Aggregation::Op::MIN:
    case Aggregation::Op::MAX:
    case Aggregation::Op::AVG:
      return TypedValue(memory);
    case Aggregation::Op::COLLECT_LIST:
      return TypedValue(TypedValue::TVector(memory));
    case Aggregation::Op::COLLECT_MAP:
      return TypedValue(TypedValue::TMap(memory));
  }
}
}  // namespace

class AggregateCursor : public Cursor {
 public:
  AggregateCursor(const Aggregate &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_.input_->MakeCursor(mem)), aggregation_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Aggregate");

    if (!pulled_all_input_) {
      ProcessAll(&frame, &context);
      pulled_all_input_ = true;
      aggregation_it_ = aggregation_.begin();

      // in case there is no input and no group_bys we need to return true
      // just this once
      if (aggregation_.empty() && self_.group_by_.empty()) {
        auto *pull_memory = context.evaluation_context.memory;
        // place default aggregation values on the frame
        for (const auto &elem : self_.aggregations_)
          frame[elem.output_sym] = DefaultAggregationOpValue(elem, pull_memory);
        // place null as remember values on the frame
        for (const Symbol &remember_sym : self_.remember_) frame[remember_sym] = TypedValue(pull_memory);
        return true;
      }
    }

    if (aggregation_it_ == aggregation_.end()) return false;

    // place aggregation values on the frame
    auto aggregation_values_it = aggregation_it_->second.values_.begin();
    for (const auto &aggregation_elem : self_.aggregations_)
      frame[aggregation_elem.output_sym] = *aggregation_values_it++;

    // place remember values on the frame
    auto remember_values_it = aggregation_it_->second.remember_.begin();
    for (const Symbol &remember_sym : self_.remember_) frame[remember_sym] = *remember_values_it++;

    ++aggregation_it_;
    return true;
  }

  bool PullMultiple(MultiFrame &multi_frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("AggregateMF");

    if (!pulled_all_input_) {
      ProcessAll(multi_frame, &context);
      pulled_all_input_ = true;
      MG_ASSERT(!multi_frame.HasValidFrame(), "ProcessAll didn't consumed all input frames!");
      aggregation_it_ = aggregation_.begin();

      // in case there is no input and no group_bys we need to return true
      // just this once
      if (aggregation_.empty() && self_.group_by_.empty()) {
        auto frame = multi_frame.GetFirstFrame();
        frame.MakeValid();
        auto *pull_memory = context.evaluation_context.memory;
        // place default aggregation values on the frame
        for (const auto &elem : self_.aggregations_) {
          frame[elem.output_sym] = DefaultAggregationOpValue(elem, pull_memory);
        }
        // place null as remember values on the frame
        for (const Symbol &remember_sym : self_.remember_) {
          frame[remember_sym] = TypedValue(pull_memory);
        }
        return true;
      }
    }

    if (aggregation_it_ == aggregation_.end()) {
      return false;
    }

    // place aggregation values on the frame
    auto &frame = multi_frame.GetFirstFrame();
    frame.MakeValid();
    auto aggregation_values_it = aggregation_it_->second.values_.begin();
    for (const auto &aggregation_elem : self_.aggregations_) {
      frame[aggregation_elem.output_sym] = *aggregation_values_it++;
    }

    // place remember values on the frame
    auto remember_values_it = aggregation_it_->second.remember_.begin();
    for (const Symbol &remember_sym : self_.remember_) {
      frame[remember_sym] = *remember_values_it++;
    }

    aggregation_it_++;
    return true;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    aggregation_.clear();
    aggregation_it_ = aggregation_.begin();
    pulled_all_input_ = false;
  }

 private:
  // Data structure for a single aggregation cache.
  // Does NOT include the group-by values since those are a key in the
  // aggregation map. The vectors in an AggregationValue contain one element for
  // each aggregation in this LogicalOp.
  struct AggregationValue {
    explicit AggregationValue(utils::MemoryResource *mem) : counts_(mem), values_(mem), remember_(mem) {}

    // how many input rows have been aggregated in respective values_ element so
    // far
    // TODO: The counting value type should be changed to an unsigned type once
    // TypedValue can support signed integer values larger than 64bits so that
    // precision isn't lost.
    utils::pmr::vector<int64_t> counts_;
    // aggregated values. Initially Null (until at least one input row with a
    // valid value gets processed)
    utils::pmr::vector<TypedValue> values_;
    // remember values.
    utils::pmr::vector<TypedValue> remember_;
  };

  const Aggregate &self_;
  const UniqueCursorPtr input_cursor_;
  // storage for aggregated data
  // map key is the vector of group-by values
  // map value is an AggregationValue struct
  utils::pmr::unordered_map<utils::pmr::vector<TypedValue>, AggregationValue,
                            // use FNV collection hashing specialized for a
                            // vector of TypedValues
                            utils::FnvCollection<utils::pmr::vector<TypedValue>, TypedValue, TypedValue::Hash>,
                            // custom equality
                            TypedValueVectorEqual>
      aggregation_;
  // iterator over the accumulated cache
  decltype(aggregation_.begin()) aggregation_it_ = aggregation_.begin();
  // this LogicalOp pulls all from the input on it's first pull
  // this switch tracks if this has been performed
  bool pulled_all_input_{false};

  /**
   * Pulls from the input operator until exhausted and aggregates the
   * results. If the input operator is not provided, a single call
   * to ProcessOne is issued.
   *
   * Accumulation automatically groups the results so that `aggregation_`
   * cache cardinality depends on number of
   * aggregation results, and not on the number of inputs.
   */
  void ProcessAll(Frame *frame, ExecutionContext *context) {
    ExpressionEvaluator evaluator(frame, context->symbol_table, context->evaluation_context, context->request_router,
                                  storage::v3::View::NEW);
    while (input_cursor_->Pull(*frame, *context)) {
      ProcessOne(*frame, &evaluator);
    }

    CalculateAverages(*context);
  }

  void ProcessAll(MultiFrame &multi_frame, ExecutionContext *context) {
    while (input_cursor_->PullMultiple(multi_frame, *context)) {
      auto valid_frames_modifier =
          multi_frame.GetValidFramesConsumer();  // consumer is needed i.o. reader because of the evaluator

      for (auto &frame : valid_frames_modifier) {
        ExpressionEvaluator evaluator(&frame, context->symbol_table, context->evaluation_context,
                                      context->request_router, storage::v3::View::NEW);
        ProcessOne(frame, &evaluator);
        frame.MakeInvalid();
      }
    }

    CalculateAverages(*context);
  }

  /**
   * Performs a single accumulation.
   */
  void ProcessOne(const Frame &frame, ExpressionEvaluator *evaluator) {
    auto *mem = aggregation_.get_allocator().GetMemoryResource();
    utils::pmr::vector<TypedValue> group_by(mem);
    group_by.reserve(self_.group_by_.size());
    for (Expression *expression : self_.group_by_) {
      group_by.emplace_back(expression->Accept(*evaluator));
    }
    auto &agg_value = aggregation_.try_emplace(std::move(group_by), mem).first->second;
    EnsureInitialized(frame, &agg_value);
    Update(evaluator, &agg_value);
  }

  void CalculateAverages(ExecutionContext &context) {
    for (size_t pos = 0; pos < self_.aggregations_.size(); ++pos) {
      if (self_.aggregations_[pos].op != Aggregation::Op::AVG) continue;
      for (auto &kv : aggregation_) {
        AggregationValue &agg_value = kv.second;
        auto count = agg_value.counts_[pos];
        auto *pull_memory = context.evaluation_context.memory;
        if (count > 0) {
          agg_value.values_[pos] = agg_value.values_[pos] / TypedValue(static_cast<double>(count), pull_memory);
        }
      }
    }
  }

  /** Ensures the new AggregationValue has been initialized. This means
   * that the value vectors are filled with an appropriate number of Nulls,
   * counts are set to 0 and remember values are remembered.
   */
  void EnsureInitialized(const Frame &frame, AggregateCursor::AggregationValue *agg_value) const {
    if (!agg_value->values_.empty()) return;

    for (const auto &agg_elem : self_.aggregations_) {
      auto *mem = agg_value->values_.get_allocator().GetMemoryResource();
      agg_value->values_.emplace_back(DefaultAggregationOpValue(agg_elem, mem));
    }
    agg_value->counts_.resize(self_.aggregations_.size(), 0);

    for (const Symbol &remember_sym : self_.remember_) agg_value->remember_.push_back(frame[remember_sym]);
  }

  /** Updates the given AggregationValue with new data. Assumes that
   * the AggregationValue has been initialized */
  void Update(ExpressionEvaluator *evaluator, AggregateCursor::AggregationValue *agg_value) {
    DMG_ASSERT(self_.aggregations_.size() == agg_value->values_.size(),
               "Expected as much AggregationValue.values_ as there are "
               "aggregations.");
    DMG_ASSERT(self_.aggregations_.size() == agg_value->counts_.size(),
               "Expected as much AggregationValue.counts_ as there are "
               "aggregations.");

    // we iterate over counts, values and aggregation info at the same time
    auto count_it = agg_value->counts_.begin();
    auto value_it = agg_value->values_.begin();
    auto agg_elem_it = self_.aggregations_.begin();
    for (; count_it < agg_value->counts_.end(); count_it++, value_it++, agg_elem_it++) {
      // COUNT(*) is the only case where input expression is optional
      // handle it here
      auto *input_expr_ptr = agg_elem_it->value;
      if (!input_expr_ptr) {
        *count_it += 1;
        *value_it = *count_it;
        continue;
      }

      TypedValue input_value = input_expr_ptr->Accept(*evaluator);

      // Aggregations skip Null input values.
      if (input_value.IsNull()) continue;
      const auto &agg_op = agg_elem_it->op;
      *count_it += 1;
      if (*count_it == 1) {
        // first value, nothing to aggregate. check type, set and continue.
        switch (agg_op) {
          case Aggregation::Op::MIN:
          case Aggregation::Op::MAX:
            *value_it = input_value;
            EnsureOkForMinMax(input_value);
            break;
          case Aggregation::Op::SUM:
          case Aggregation::Op::AVG:
            *value_it = input_value;
            EnsureOkForAvgSum(input_value);
            break;
          case Aggregation::Op::COUNT:
            *value_it = 1;
            break;
          case Aggregation::Op::COLLECT_LIST:
            value_it->ValueList().push_back(input_value);
            break;
          case Aggregation::Op::COLLECT_MAP:
            auto key = agg_elem_it->key->Accept(*evaluator);
            if (key.type() != TypedValue::Type::String) throw QueryRuntimeException("Map key must be a string.");
            value_it->ValueMap().emplace(key.ValueString(), input_value);
            break;
        }
        continue;
      }

      // aggregation of existing values
      switch (agg_op) {
        case Aggregation::Op::COUNT:
          *value_it = *count_it;
          break;
        case Aggregation::Op::MIN: {
          EnsureOkForMinMax(input_value);
          try {
            TypedValue comparison_result = input_value < *value_it;
            // since we skip nulls we either have a valid comparison, or
            // an exception was just thrown above
            // safe to assume a bool TypedValue
            if (comparison_result.ValueBool()) *value_it = input_value;
          } catch (const expr::TypedValueException &) {
            throw QueryRuntimeException("Unable to get MIN of '{}' and '{}'.", input_value.type(), value_it->type());
          }
          break;
        }
        case Aggregation::Op::MAX: {
          //  all comments as for Op::Min
          EnsureOkForMinMax(input_value);
          try {
            TypedValue comparison_result = input_value > *value_it;
            if (comparison_result.ValueBool()) *value_it = input_value;
          } catch (const expr::TypedValueException &) {
            throw QueryRuntimeException("Unable to get MAX of '{}' and '{}'.", input_value.type(), value_it->type());
          }
          break;
        }
        case Aggregation::Op::AVG:
        // for averaging we sum first and divide by count once all
        // the input has been processed
        case Aggregation::Op::SUM:
          EnsureOkForAvgSum(input_value);
          *value_it = *value_it + input_value;
          break;
        case Aggregation::Op::COLLECT_LIST:
          value_it->ValueList().push_back(input_value);
          break;
        case Aggregation::Op::COLLECT_MAP:
          auto key = agg_elem_it->key->Accept(*evaluator);
          if (key.type() != TypedValue::Type::String) throw QueryRuntimeException("Map key must be a string.");
          value_it->ValueMap().emplace(key.ValueString(), input_value);
          break;
      }  // end switch over Aggregation::Op enum
    }    // end loop over all aggregations
  }

  /** Checks if the given TypedValue is legal in MIN and MAX. If not
   * an appropriate exception is thrown. */
  static void EnsureOkForMinMax(const TypedValue &value) {
    switch (value.type()) {
      case TypedValue::Type::Bool:
      case TypedValue::Type::Int:
      case TypedValue::Type::Double:
      case TypedValue::Type::String:
        return;
      default:
        throw QueryRuntimeException(
            "Only boolean, numeric and string values are allowed in "
            "MIN and MAX aggregations.");
    }
  }

  /** Checks if the given TypedValue is legal in AVG and SUM. If not
   * an appropriate exception is thrown. */
  static void EnsureOkForAvgSum(const TypedValue &value) {
    switch (value.type()) {
      case TypedValue::Type::Int:
      case TypedValue::Type::Double:
        return;
      default:
        throw QueryRuntimeException("Only numeric values allowed in SUM and AVG aggregations.");
    }
  }
};

UniqueCursorPtr Aggregate::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::AggregateOperator);

  return MakeUniqueCursorPtr<AggregateCursor>(mem, *this, mem);
}

Skip::Skip(const std::shared_ptr<LogicalOperator> &input, Expression *expression)
    : input_(input), expression_(expression) {}

ACCEPT_WITH_INPUT(Skip)

UniqueCursorPtr Skip::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::SkipOperator);

  return MakeUniqueCursorPtr<SkipCursor>(mem, *this, mem);
}

std::vector<Symbol> Skip::OutputSymbols(const SymbolTable &symbol_table) const {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

std::vector<Symbol> Skip::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Skip::SkipCursor::SkipCursor(const Skip &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Skip::SkipCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Skip");

  while (input_cursor_->Pull(frame, context)) {
    if (to_skip_ == -1) {
      // First successful pull from the input, evaluate the skip expression.
      // The skip expression doesn't contain identifiers so graph view
      // parameter is not important.
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                    storage::v3::View::OLD);
      TypedValue to_skip = self_.expression_->Accept(evaluator);
      if (to_skip.type() != TypedValue::Type::Int)
        throw QueryRuntimeException("Number of elements to skip must be an integer.");

      to_skip_ = to_skip.ValueInt();
      if (to_skip_ < 0) throw QueryRuntimeException("Number of elements to skip must be non-negative.");
    }

    if (skipped_++ < to_skip_) continue;
    return true;
  }
  return false;
}

void Skip::SkipCursor::Shutdown() { input_cursor_->Shutdown(); }

void Skip::SkipCursor::Reset() {
  input_cursor_->Reset();
  to_skip_ = -1;
  skipped_ = 0;
}

Limit::Limit(const std::shared_ptr<LogicalOperator> &input, Expression *expression)
    : input_(input), expression_(expression) {}

ACCEPT_WITH_INPUT(Limit)

UniqueCursorPtr Limit::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::LimitOperator);

  return MakeUniqueCursorPtr<LimitCursor>(mem, *this, mem);
}

std::vector<Symbol> Limit::OutputSymbols(const SymbolTable &symbol_table) const {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

std::vector<Symbol> Limit::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Limit::LimitCursor::LimitCursor(const Limit &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Limit::LimitCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Limit");

  // We need to evaluate the limit expression before the first input Pull
  // because it might be 0 and thereby we shouldn't Pull from input at all.
  // We can do this before Pulling from the input because the limit expression
  // is not allowed to contain any identifiers.
  if (limit_ == -1) {
    // Limit expression doesn't contain identifiers so graph view is not
    // important.
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                  storage::v3::View::OLD);
    TypedValue limit = self_.expression_->Accept(evaluator);
    if (limit.type() != TypedValue::Type::Int)
      throw QueryRuntimeException("Limit on number of returned elements must be an integer.");

    limit_ = limit.ValueInt();
    if (limit_ < 0) throw QueryRuntimeException("Limit on number of returned elements must be non-negative.");
  }

  // check we have not exceeded the limit before pulling
  if (pulled_++ >= limit_) return false;

  return input_cursor_->Pull(frame, context);
}

void Limit::LimitCursor::Shutdown() { input_cursor_->Shutdown(); }

void Limit::LimitCursor::Reset() {
  input_cursor_->Reset();
  limit_ = -1;
  pulled_ = 0;
}

OrderBy::OrderBy(const std::shared_ptr<LogicalOperator> &input, const std::vector<SortItem> &order_by,
                 const std::vector<Symbol> &output_symbols)
    : input_(input), output_symbols_(output_symbols) {
  // split the order_by vector into two vectors of orderings and expressions
  std::vector<Ordering> ordering;
  ordering.reserve(order_by.size());
  order_by_.reserve(order_by.size());
  for (const auto &ordering_expression_pair : order_by) {
    ordering.emplace_back(ordering_expression_pair.ordering);
    order_by_.emplace_back(ordering_expression_pair.expression);
  }
  compare_ = TypedValueVectorCompare(ordering);
}

ACCEPT_WITH_INPUT(OrderBy)

std::vector<Symbol> OrderBy::OutputSymbols(const SymbolTable &symbol_table) const {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

std::vector<Symbol> OrderBy::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

class OrderByCursor : public Cursor {
 public:
  OrderByCursor(const OrderBy &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_.input_->MakeCursor(mem)), cache_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("OrderBy");

    if (!did_pull_all_) {
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                    storage::v3::View::OLD);
      auto *mem = cache_.get_allocator().GetMemoryResource();
      while (input_cursor_->Pull(frame, context)) {
        // collect the order_by elements
        utils::pmr::vector<TypedValue> order_by(mem);
        order_by.reserve(self_.order_by_.size());
        for (auto expression_ptr : self_.order_by_) {
          order_by.emplace_back(expression_ptr->Accept(evaluator));
        }

        // collect the output elements
        utils::pmr::vector<TypedValue> output(mem);
        output.reserve(self_.output_symbols_.size());
        for (const Symbol &output_sym : self_.output_symbols_) output.emplace_back(frame[output_sym]);

        cache_.push_back(Element{std::move(order_by), std::move(output)});
      }

      std::sort(cache_.begin(), cache_.end(), [this](const auto &pair1, const auto &pair2) {
        return self_.compare_(pair1.order_by, pair2.order_by);
      });

      did_pull_all_ = true;
      cache_it_ = cache_.begin();
    }

    if (cache_it_ == cache_.end()) return false;

    if (MustAbort(context)) throw HintedAbortError();

    // place the output values on the frame
    DMG_ASSERT(self_.output_symbols_.size() == cache_it_->remember.size(),
               "Number of values does not match the number of output symbols "
               "in OrderBy");
    auto output_sym_it = self_.output_symbols_.begin();
    for (const TypedValue &output : cache_it_->remember) frame[*output_sym_it++] = output;

    cache_it_++;
    return true;
  }
  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    did_pull_all_ = false;
    cache_.clear();
    cache_it_ = cache_.begin();
  }

 private:
  struct Element {
    utils::pmr::vector<TypedValue> order_by;
    utils::pmr::vector<TypedValue> remember;
  };

  const OrderBy &self_;
  const UniqueCursorPtr input_cursor_;
  bool did_pull_all_{false};
  // a cache of elements pulled from the input
  // the cache is filled and sorted (only on first elem) on first Pull
  utils::pmr::vector<Element> cache_;
  // iterator over the cache_, maintains state between Pulls
  decltype(cache_.begin()) cache_it_ = cache_.begin();
};

UniqueCursorPtr OrderBy::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::OrderByOperator);

  return MakeUniqueCursorPtr<OrderByCursor>(mem, *this, mem);
}

Merge::Merge(const std::shared_ptr<LogicalOperator> &input, const std::shared_ptr<LogicalOperator> &merge_match,
             const std::shared_ptr<LogicalOperator> &merge_create)
    : input_(input ? input : std::make_shared<Once>()), merge_match_(merge_match), merge_create_(merge_create) {}

bool Merge::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    input_->Accept(visitor) && merge_match_->Accept(visitor) && merge_create_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

UniqueCursorPtr Merge::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::MergeOperator);

  return MakeUniqueCursorPtr<MergeCursor>(mem, *this, mem);
}

std::vector<Symbol> Merge::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  // Match and create branches should have the same symbols, so just take one
  // of them.
  auto my_symbols = merge_match_->OutputSymbols(table);
  symbols.insert(symbols.end(), my_symbols.begin(), my_symbols.end());
  return symbols;
}

Merge::MergeCursor::MergeCursor(const Merge &self, utils::MemoryResource *mem)
    : input_cursor_(self.input_->MakeCursor(mem)),
      merge_match_cursor_(self.merge_match_->MakeCursor(mem)),
      merge_create_cursor_(self.merge_create_->MakeCursor(mem)) {}

bool Merge::MergeCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Merge");

  while (true) {
    if (pull_input_) {
      if (input_cursor_->Pull(frame, context)) {
        // after a successful input from the input
        // reset merge_match (it's expand iterators maintain state)
        // and merge_create (could have a Once at the beginning)
        merge_match_cursor_->Reset();
        merge_create_cursor_->Reset();
      } else
        // input is exhausted, we're done
        return false;
    }

    // pull from the merge_match cursor
    if (merge_match_cursor_->Pull(frame, context)) {
      // if successful, next Pull from this should not pull_input_
      pull_input_ = false;
      return true;
    } else {
      // failed to Pull from the merge_match cursor
      if (pull_input_) {
        // if we have just now pulled from the input
        // and failed to pull from merge_match, we should create
        __attribute__((unused)) bool merge_create_pull_result = merge_create_cursor_->Pull(frame, context);
        DMG_ASSERT(merge_create_pull_result, "MergeCreate must never fail");
        return true;
      }
      // We have exhausted merge_match_cursor_ after 1 or more successful
      // Pulls. Attempt next input_cursor_ pull
      pull_input_ = true;
      continue;
    }
  }
}

void Merge::MergeCursor::Shutdown() {
  input_cursor_->Shutdown();
  merge_match_cursor_->Shutdown();
  merge_create_cursor_->Shutdown();
}

void Merge::MergeCursor::Reset() {
  input_cursor_->Reset();
  merge_match_cursor_->Reset();
  merge_create_cursor_->Reset();
  pull_input_ = true;
}

Optional::Optional(const std::shared_ptr<LogicalOperator> &input, const std::shared_ptr<LogicalOperator> &optional,
                   const std::vector<Symbol> &optional_symbols)
    : input_(input ? input : std::make_shared<Once>()), optional_(optional), optional_symbols_(optional_symbols) {}

bool Optional::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    input_->Accept(visitor) && optional_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

class OptionalCursor;

UniqueCursorPtr Optional::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::OptionalOperator);

  return MakeUniqueCursorPtr<OptionalCursor>(mem, *this, mem);
}

std::vector<Symbol> Optional::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  auto my_symbols = optional_->ModifiedSymbols(table);
  symbols.insert(symbols.end(), my_symbols.begin(), my_symbols.end());
  return symbols;
}

class OptionalCursor : public Cursor {
 public:
  OptionalCursor(const Optional &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self.input_->MakeCursor(mem)), optional_cursor_(self.optional_->MakeCursor(mem)) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Optional");

    while (true) {
      if (pull_input_) {
        if (input_cursor_->Pull(frame, context)) {
          // after a successful pull from the input
          // reset optional_ (it's expand iterators maintain state)
          optional_cursor_->Reset();
        } else
          // input is exhausted, we're done
          return false;
      }

      // pull from the optional_ cursor
      if (optional_cursor_->Pull(frame, context)) {
        // if successful, next Pull from this should not pull_input_
        pull_input_ = false;
        return true;
      }
      // failed to Pull from the merge_match cursor
      if (pull_input_) {
        // if we have just now pulled from the input
        // and failed to pull from optional_ so set the
        // optional symbols to Null, ensure next time the
        // input gets pulled and return true
        for (const Symbol &sym : self_.optional_symbols_) frame[sym] = TypedValue(context.evaluation_context.memory);
        pull_input_ = true;
        return true;
      }
      // we have exhausted optional_cursor_ after 1 or more successful Pulls
      // attempt next input_cursor_ pull
      pull_input_ = true;
    }
  }

  bool HandleReadyInput(InvalidFramesPopulator &output_populator, InvalidFramesPopulator::Iterator &output_frames_it,
                        ExecutionContext &context) {
    bool populated_any = false;
    while (true) {
      switch (optional_state_) {
        case State::Pull: {
          if (!optional_cursor_->PullMultiple(*optional_multi_frame_, context)) {
            optional_state_ = State::Exhausted;
            optional_frames_consumer_.reset();
            optional_frames_it_ = {};
            if (populated_any) {
              ++own_frames_it_;
            }
          } else {
            optional_frames_consumer_ = optional_multi_frame_->GetValidFramesConsumer();
            optional_frames_it_ = optional_frames_consumer_->begin();
            optional_state_ = State::Ready;
          }
          break;
        }
        case State::Ready: {
          while (optional_frames_it_ != optional_frames_consumer_->end()) {
            if (output_frames_it == output_populator.end()) {
              return populated_any;
            }
            populated_any = true;
            if (optional_frames_it_->Id() == own_frames_it_->Id()) {
              // This might be a move, but then in we have to have special logic is EnsureOwnMultiFramesAreGood
              *output_frames_it = *optional_frames_it_;
              last_matched_frame_ = optional_frames_it_->Id();
              optional_frames_it_->MakeInvalid();
              ++optional_frames_it_;
              ++output_frames_it;
              if (optional_frames_it_ == optional_frames_consumer_->end()) {
                optional_state_ = State::Pull;
              }
            } else if (last_matched_frame_ == own_frames_it_->Id()) {
              ++own_frames_it_;
            } else {
              // TODO(antaljanosbenjamin): Remove (or improve the message of) this assert
              MG_ASSERT(optional_frames_it_->Id() > own_frames_it_->Id(), "This should be the case DELETE ME");
              for (const auto &symbol : self_.optional_symbols_) {
                spdlog::error("{}", symbol.name());
                (*own_frames_it_)[symbol] = TypedValue(context.evaluation_context.memory);
              }
              // This might be a move, but then in we have to have special logic is EnsureOwnMultiFramesAreGood
              *output_frames_it = *own_frames_it_;
              last_matched_frame_ = own_frames_it_->Id();
              own_frames_it_->MakeInvalid();
              ++own_frames_it_;
            }
          }
          break;
        }
        case State::Exhausted: {
          while (own_frames_it_ != own_frames_consumer_->end() && output_frames_it != output_populator.end()) {
            MG_ASSERT(!optional_frames_consumer_.has_value(), "This should be the case DELETE ME");
            for (const auto &symbol : self_.optional_symbols_) {
              spdlog::error("{}", symbol.name());
              (*own_frames_it_)[symbol] = TypedValue(context.evaluation_context.memory);
            }
            // This might be a move, but then in we have to have special logic is EnsureOwnMultiFramesAreGood
            *output_frames_it = *own_frames_it_;
            ++own_frames_it_;

            populated_any = true;
            own_frames_it_->MakeInvalid();
            ++output_frames_it;
          }
          return populated_any;
        }
      }
    }
    return populated_any;
  }

  bool PullMultiple(MultiFrame &output_multi_frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("OptionalMF");

    EnsureOwnMultiFramesAreGood(output_multi_frame);
    auto populated_any{false};

    auto output_frames_populator = output_multi_frame.GetInvalidFramesPopulator();
    auto output_frames_it = output_frames_populator.begin();
    while (true) {
      switch (input_state_) {
        case State::Pull: {
          MG_ASSERT(optional_state_ != State::Ready, "Unexpected state");
          if (!input_cursor_->PullMultiple(*own_multi_frame_, context)) {
            input_state_ = State::Exhausted;
            optional_state_ = State::Exhausted;
          } else {
            input_state_ = State::Ready;
            optional_state_ = State::Pull;
            uint64_t frame_id{0U};
            for (auto &frame : own_multi_frame_->GetValidFramesModifier()) {
              frame.SetId(frame_id++);
            }
            last_matched_frame_ = 0U;
            optional_cursor_->Reset();
            optional_cursor_->PushDown(*own_multi_frame_);
            own_frames_consumer_ = own_multi_frame_->GetValidFramesConsumer();
            own_frames_it_ = own_frames_consumer_->begin();
          }
          break;
        }
        case State::Ready: {
          populated_any |= HandleReadyInput(output_frames_populator, output_frames_it, context);
          if (output_frames_it == output_frames_populator.end()) {
            return populated_any;
          }
          if (own_frames_it_ == own_frames_consumer_->end()) {
            input_state_ = State::Pull;
          }
          break;
        }
        case State::Exhausted: {
          MG_ASSERT(optional_state_ == State::Exhausted);
          return populated_any;
        }
      }
    }
  }

  void Shutdown() override {
    input_cursor_->Shutdown();
    optional_cursor_->Shutdown();
  }

  void Reset() override {
    // TODO(antaljanosbenjamin)
    input_cursor_->Reset();
    optional_cursor_->Reset();
    pull_input_ = true;
  }

 private:
  enum class State { Pull, Ready, Exhausted };

  void EnsureOwnMultiFramesAreGood(MultiFrame &output_multi_frame) {
    if (!own_multi_frame_.has_value()) {
      own_multi_frame_.emplace(MultiFrame(output_multi_frame.GetFirstFrame().Elems().size(),
                                          FLAGS_default_multi_frame_size, output_multi_frame.GetMemoryResource()));
      own_frames_consumer_.emplace(own_multi_frame_->GetValidFramesConsumer());
      own_frames_it_ = own_frames_consumer_->begin();
      optional_multi_frame_.emplace(MultiFrame(output_multi_frame.GetFirstFrame().Elems().size(),
                                               FLAGS_default_multi_frame_size, output_multi_frame.GetMemoryResource()));
    }
    MG_ASSERT(output_multi_frame.GetFirstFrame().Elems().size() == own_multi_frame_->GetFirstFrame().Elems().size());
  }

  const Optional &self_;
  const UniqueCursorPtr input_cursor_;
  const UniqueCursorPtr optional_cursor_;

  State input_state_{State::Pull};
  State optional_state_{State::Pull};
  std::optional<MultiFrame> own_multi_frame_;
  std::optional<ValidFramesConsumer> own_frames_consumer_;
  ValidFramesConsumer::Iterator own_frames_it_;
  std::optional<MultiFrame> optional_multi_frame_;
  std::optional<ValidFramesConsumer> optional_frames_consumer_;
  ValidFramesConsumer::Iterator optional_frames_it_;
  uint64_t last_matched_frame_{0U};
  // indicates if the next Pull from this cursor should
  // perform a Pull from the input_cursor_
  // this is true when:
  //  - first pulling from this Cursor
  //  - previous Pull from this cursor exhausted the optional_cursor_
  bool pull_input_{true};
};

Unwind::Unwind(const std::shared_ptr<LogicalOperator> &input, Expression *input_expression, Symbol output_symbol)
    : input_(input ? input : std::make_shared<Once>()),
      input_expression_(input_expression),
      output_symbol_(output_symbol) {}

ACCEPT_WITH_INPUT(Unwind)

std::vector<Symbol> Unwind::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(output_symbol_);
  return symbols;
}

class UnwindCursor : public Cursor {
 public:
  UnwindCursor(const Unwind &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self.input_->MakeCursor(mem)), input_value_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Unwind");
    while (true) {
      if (MustAbort(context)) throw HintedAbortError();
      // if we reached the end of our list of values
      // pull from the input
      if (input_value_it_ == input_value_.end()) {
        if (!input_cursor_->Pull(frame, context)) return false;

        // successful pull from input, initialize value and iterator
        SetInputValue(frame, context);
      }

      // if we reached the end of our list of values goto back to top
      if (input_value_it_ == input_value_.end()) continue;

      frame[self_.output_symbol_] = *input_value_it_++;
      return true;
    }
  }

  bool PullMultiple(MultiFrame &output_multi_frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("UnwindMF");

    if (!own_multi_frame_.has_value()) {
      own_multi_frame_.emplace(MultiFrame(output_multi_frame.GetFirstFrame().Elems().size(),
                                          FLAGS_default_multi_frame_size, output_multi_frame.GetMemoryResource()));
      own_frames_consumer_.emplace(own_multi_frame_->GetValidFramesConsumer());
      own_frames_it_ = own_frames_consumer_->begin();
    }

    auto output_frames_populator = output_multi_frame.GetInvalidFramesPopulator();
    auto populated_any = false;

    while (true) {
      switch (state_) {
        case State::PullInput: {
          if (!input_cursor_->PullMultiple(*own_multi_frame_, context)) {
            state_ = State::Exhausted;
            return populated_any;
          }
          own_frames_consumer_.emplace(own_multi_frame_->GetValidFramesConsumer());
          own_frames_it_ = own_frames_consumer_->begin();
          state_ = State::InitializeInputValue;
          break;
        }
        case State::InitializeInputValue: {
          if (own_frames_it_ == own_frames_consumer_->end()) {
            state_ = State::PullInput;
            continue;
          }
          SetInputValue(*own_frames_it_, context);
          state_ = State::PopulateOutput;
          break;
        }
        case State::PopulateOutput: {
          if (!output_multi_frame.HasInvalidFrame()) {
            return populated_any;
          }
          if (input_value_it_ == input_value_.end()) {
            own_frames_it_->MakeInvalid();
            ++own_frames_it_;
            state_ = State::InitializeInputValue;
            continue;
          }

          for (auto output_frame_it = output_frames_populator.begin();
               output_frame_it != output_frames_populator.end() && input_value_it_ != input_value_.end();
               ++output_frame_it) {
            auto &output_frame = *output_frame_it;
            output_frame = *own_frames_it_;
            output_frame[self_.output_symbol_] = std::move(*input_value_it_);
            input_value_it_++;
            populated_any = true;
          }
          break;
        }
        case State::Exhausted: {
          return populated_any;
        }
      }
    }
    return populated_any;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    input_value_.clear();
    input_value_it_ = input_value_.end();
  }

  void SetInputValue(Frame &frame, ExecutionContext &context) {
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                  storage::v3::View::OLD);
    TypedValue input_value = self_.input_expression_->Accept(evaluator);
    if (input_value.type() != TypedValue::Type::List) {
      throw QueryRuntimeException("Argument of UNWIND must be a list, but '{}' was provided.", input_value.type());
    }
    // It would be nice if we could move it, however it can be tricky to make it work because of allocators and
    // different memory resources, be careful.
    input_value_ = std::move(input_value.ValueList());
    input_value_it_ = input_value_.begin();
  }

 private:
  using InputVector = utils::pmr::vector<TypedValue>;
  using InputIterator = InputVector::iterator;

  const Unwind &self_;
  const UniqueCursorPtr input_cursor_;
  // typed values we are unwinding and yielding
  InputVector input_value_;
  // current position in input_value_
  InputIterator input_value_it_ = input_value_.end();

  enum class State { PullInput, InitializeInputValue, PopulateOutput, Exhausted };

  State state_{State::PullInput};
  std::optional<MultiFrame> own_multi_frame_;
  std::optional<ValidFramesConsumer> own_frames_consumer_;
  ValidFramesConsumer::Iterator own_frames_it_;
};

UniqueCursorPtr Unwind::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::UnwindOperator);

  return MakeUniqueCursorPtr<UnwindCursor>(mem, *this, mem);
}

class DistinctCursor : public Cursor {
 public:
  DistinctCursor(const Distinct &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self.input_->MakeCursor(mem)), seen_rows_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Distinct");

    while (true) {
      if (!input_cursor_->Pull(frame, context)) return false;

      utils::pmr::vector<TypedValue> row(seen_rows_.get_allocator().GetMemoryResource());
      row.reserve(self_.value_symbols_.size());
      for (const auto &symbol : self_.value_symbols_) row.emplace_back(frame[symbol]);
      if (seen_rows_.insert(std::move(row)).second) return true;
    }
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    seen_rows_.clear();
  }

 private:
  const Distinct &self_;
  const UniqueCursorPtr input_cursor_;
  // a set of already seen rows
  utils::pmr::unordered_set<utils::pmr::vector<TypedValue>,
                            // use FNV collection hashing specialized for a
                            // vector of TypedValue
                            utils::FnvCollection<utils::pmr::vector<TypedValue>, TypedValue, TypedValue::Hash>,
                            TypedValueVectorEqual>
      seen_rows_;
};

Distinct::Distinct(const std::shared_ptr<LogicalOperator> &input, const std::vector<Symbol> &value_symbols)
    : input_(input ? input : std::make_shared<Once>()), value_symbols_(value_symbols) {}

ACCEPT_WITH_INPUT(Distinct)

UniqueCursorPtr Distinct::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::DistinctOperator);

  return MakeUniqueCursorPtr<DistinctCursor>(mem, *this, mem);
}

std::vector<Symbol> Distinct::OutputSymbols(const SymbolTable &symbol_table) const {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

std::vector<Symbol> Distinct::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Union::Union(const std::shared_ptr<LogicalOperator> &left_op, const std::shared_ptr<LogicalOperator> &right_op,
             const std::vector<Symbol> &union_symbols, const std::vector<Symbol> &left_symbols,
             const std::vector<Symbol> &right_symbols)
    : left_op_(left_op),
      right_op_(right_op),
      union_symbols_(union_symbols),
      left_symbols_(left_symbols),
      right_symbols_(right_symbols) {}

UniqueCursorPtr Union::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::UnionOperator);

  return MakeUniqueCursorPtr<Union::UnionCursor>(mem, *this, mem);
}

bool Union::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    if (left_op_->Accept(visitor)) {
      right_op_->Accept(visitor);
    }
  }
  return visitor.PostVisit(*this);
}

std::vector<Symbol> Union::OutputSymbols(const SymbolTable &) const { return union_symbols_; }

std::vector<Symbol> Union::ModifiedSymbols(const SymbolTable &) const { return union_symbols_; }

WITHOUT_SINGLE_INPUT(Union);

Union::UnionCursor::UnionCursor(const Union &self, utils::MemoryResource *mem)
    : self_(self), left_cursor_(self.left_op_->MakeCursor(mem)), right_cursor_(self.right_op_->MakeCursor(mem)) {}

bool Union::UnionCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Union");

  utils::pmr::unordered_map<std::string, TypedValue> results(context.evaluation_context.memory);
  if (left_cursor_->Pull(frame, context)) {
    // collect values from the left child
    for (const auto &output_symbol : self_.left_symbols_) {
      results[output_symbol.name()] = frame[output_symbol];
    }
  } else if (right_cursor_->Pull(frame, context)) {
    // collect values from the right child
    for (const auto &output_symbol : self_.right_symbols_) {
      results[output_symbol.name()] = frame[output_symbol];
    }
  } else {
    return false;
  }

  // put collected values on frame under union symbols
  for (const auto &symbol : self_.union_symbols_) {
    frame[symbol] = results[symbol.name()];
  }
  return true;
}

void Union::UnionCursor::Shutdown() {
  left_cursor_->Shutdown();
  right_cursor_->Shutdown();
}

void Union::UnionCursor::Reset() {
  left_cursor_->Reset();
  right_cursor_->Reset();
}

std::vector<Symbol> Cartesian::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = left_op_->ModifiedSymbols(table);
  auto right = right_op_->ModifiedSymbols(table);
  symbols.insert(symbols.end(), right.begin(), right.end());
  return symbols;
}

bool Cartesian::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    left_op_->Accept(visitor) && right_op_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

WITHOUT_SINGLE_INPUT(Cartesian);

namespace {

class CartesianCursor : public Cursor {
 public:
  CartesianCursor(const Cartesian &self, utils::MemoryResource *mem)
      : self_(self),
        left_op_frames_(mem),
        right_op_frame_(mem),
        left_op_cursor_(self.left_op_->MakeCursor(mem)),
        right_op_cursor_(self_.right_op_->MakeCursor(mem)) {
    MG_ASSERT(left_op_cursor_ != nullptr, "CartesianCursor: Missing left operator cursor.");
    MG_ASSERT(right_op_cursor_ != nullptr, "CartesianCursor: Missing right operator cursor.");
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Cartesian");

    if (!cartesian_pull_initialized_) {
      // Pull all left_op frames.
      while (left_op_cursor_->Pull(frame, context)) {
        left_op_frames_.emplace_back(frame.Elems().begin(), frame.Elems().end());
      }

      // We're setting the iterator to 'end' here so it pulls the right
      // cursor.
      left_op_frames_it_ = left_op_frames_.end();
      cartesian_pull_initialized_ = true;
    }

    // If left operator yielded zero results there is no cartesian product.
    if (left_op_frames_.empty()) {
      return false;
    }

    auto restore_frame = [&frame](const auto &symbols, const auto &restore_from) {
      for (const auto &symbol : symbols) {
        frame[symbol] = restore_from[symbol.position()];
      }
    };

    if (left_op_frames_it_ == left_op_frames_.end()) {
      // Advance right_op_cursor_.
      if (!right_op_cursor_->Pull(frame, context)) return false;

      right_op_frame_.assign(frame.Elems().begin(), frame.Elems().end());
      left_op_frames_it_ = left_op_frames_.begin();
    } else {
      // Make sure right_op_cursor last pulled results are on frame.
      restore_frame(self_.right_symbols_, right_op_frame_);
    }

    if (MustAbort(context)) throw HintedAbortError();

    restore_frame(self_.left_symbols_, *left_op_frames_it_);
    left_op_frames_it_++;
    return true;
  }

  void Shutdown() override {
    left_op_cursor_->Shutdown();
    right_op_cursor_->Shutdown();
  }

  void Reset() override {
    left_op_cursor_->Reset();
    right_op_cursor_->Reset();
    right_op_frame_.clear();
    left_op_frames_.clear();
    left_op_frames_it_ = left_op_frames_.end();
    cartesian_pull_initialized_ = false;
  }

 private:
  const Cartesian &self_;
  utils::pmr::vector<utils::pmr::vector<TypedValue>> left_op_frames_;
  utils::pmr::vector<TypedValue> right_op_frame_;
  const UniqueCursorPtr left_op_cursor_;
  const UniqueCursorPtr right_op_cursor_;
  utils::pmr::vector<utils::pmr::vector<TypedValue>>::iterator left_op_frames_it_;
  bool cartesian_pull_initialized_{false};
};

}  // namespace

UniqueCursorPtr Cartesian::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::CartesianOperator);

  return MakeUniqueCursorPtr<CartesianCursor>(mem, *this, mem);
}

OutputTable::OutputTable(std::vector<Symbol> output_symbols, std::vector<std::vector<TypedValue>> rows)
    : output_symbols_(std::move(output_symbols)), callback_([rows](Frame *, ExecutionContext *) { return rows; }) {}

OutputTable::OutputTable(std::vector<Symbol> output_symbols,
                         std::function<std::vector<std::vector<TypedValue>>(Frame *, ExecutionContext *)> callback)
    : output_symbols_(std::move(output_symbols)), callback_(std::move(callback)) {}

WITHOUT_SINGLE_INPUT(OutputTable);

class OutputTableCursor : public Cursor {
 public:
  OutputTableCursor(const OutputTable &self) : self_(self) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    if (!pulled_) {
      rows_ = self_.callback_(&frame, &context);
      for (const auto &row : rows_) {
        MG_ASSERT(row.size() == self_.output_symbols_.size(), "Wrong number of columns in row!");
      }
      pulled_ = true;
    }
    if (current_row_ < rows_.size()) {
      for (size_t i = 0; i < self_.output_symbols_.size(); ++i) {
        frame[self_.output_symbols_[i]] = rows_[current_row_][i];
      }
      current_row_++;
      return true;
    }
    return false;
  }

  void Reset() override {
    pulled_ = false;
    current_row_ = 0;
    rows_.clear();
  }

  void Shutdown() override {}

 private:
  const OutputTable &self_;
  size_t current_row_{0};
  std::vector<std::vector<TypedValue>> rows_;
  bool pulled_{false};
};

UniqueCursorPtr OutputTable::MakeCursor(utils::MemoryResource *mem) const {
  return MakeUniqueCursorPtr<OutputTableCursor>(mem, *this);
}

OutputTableStream::OutputTableStream(
    std::vector<Symbol> output_symbols,
    std::function<std::optional<std::vector<TypedValue>>(Frame *, ExecutionContext *)> callback)
    : output_symbols_(std::move(output_symbols)), callback_(std::move(callback)) {}

WITHOUT_SINGLE_INPUT(OutputTableStream);

class OutputTableStreamCursor : public Cursor {
 public:
  explicit OutputTableStreamCursor(const OutputTableStream *self) : self_(self) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    const auto row = self_->callback_(&frame, &context);
    if (row) {
      MG_ASSERT(row->size() == self_->output_symbols_.size(), "Wrong number of columns in row!");
      for (size_t i = 0; i < self_->output_symbols_.size(); ++i) {
        frame[self_->output_symbols_[i]] = row->at(i);
      }
      return true;
    }
    return false;
  }

  // TODO(tsabolcec): Come up with better approach for handling `Reset()`.
  // One possibility is to implement a custom closure utility class with
  // `Reset()` method.
  void Reset() override { throw utils::NotYetImplemented("OutputTableStreamCursor::Reset"); }

  void Shutdown() override {}

 private:
  const OutputTableStream *self_;
};

UniqueCursorPtr OutputTableStream::MakeCursor(utils::MemoryResource *mem) const {
  return MakeUniqueCursorPtr<OutputTableStreamCursor>(mem, this);
}

CallProcedure::CallProcedure(std::shared_ptr<LogicalOperator> input, std::string name, std::vector<Expression *> args,
                             std::vector<std::string> fields, std::vector<Symbol> symbols, Expression *memory_limit,
                             size_t memory_scale, bool is_write)
    : input_(input ? input : std::make_shared<Once>()),
      procedure_name_(name),
      arguments_(args),
      result_fields_(fields),
      result_symbols_(symbols),
      memory_limit_(memory_limit),
      memory_scale_(memory_scale),
      is_write_(is_write) {}

ACCEPT_WITH_INPUT(CallProcedure);

std::vector<Symbol> CallProcedure::OutputSymbols(const SymbolTable &) const { return result_symbols_; }

std::vector<Symbol> CallProcedure::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.insert(symbols.end(), result_symbols_.begin(), result_symbols_.end());
  return symbols;
}

void CallProcedure::IncrementCounter(const std::string &procedure_name) {
  procedure_counters_.WithLock([&](auto &counters) { ++counters[procedure_name]; });
}

std::unordered_map<std::string, int64_t> CallProcedure::GetAndResetCounters() {
  auto counters = procedure_counters_.Lock();
  auto ret = std::move(*counters);
  counters->clear();
  return ret;
}

UniqueCursorPtr CallProcedure::MakeCursor(utils::MemoryResource *mem) const {
  throw QueryRuntimeException("Procedure call is not supported!");
}

LoadCsv::LoadCsv(std::shared_ptr<LogicalOperator> input, Expression *file, bool with_header, bool ignore_bad,
                 Expression *delimiter, Expression *quote, Symbol row_var)
    : input_(input ? input : (std::make_shared<Once>())),
      file_(file),
      with_header_(with_header),
      ignore_bad_(ignore_bad),
      delimiter_(delimiter),
      quote_(quote),
      row_var_(row_var) {
  MG_ASSERT(file_, "Something went wrong - '{}' member file_ shouldn't be a nullptr", __func__);
}

bool LoadCsv::Accept(HierarchicalLogicalOperatorVisitor &visitor) { return false; };

class LoadCsvCursor;

std::vector<Symbol> LoadCsv::OutputSymbols(const SymbolTable &sym_table) const { return {row_var_}; };

std::vector<Symbol> LoadCsv::ModifiedSymbols(const SymbolTable &sym_table) const {
  auto symbols = input_->ModifiedSymbols(sym_table);
  symbols.push_back(row_var_);
  return symbols;
};

namespace {
// copy-pasted from interpreter.cpp
TypedValue EvaluateOptionalExpression(Expression *expression, ExpressionEvaluator *eval) {
  return expression ? expression->Accept(*eval) : TypedValue();
}

auto ToOptionalString(ExpressionEvaluator *evaluator, Expression *expression) -> std::optional<utils::pmr::string> {
  const auto evaluated_expr = EvaluateOptionalExpression(expression, evaluator);
  if (evaluated_expr.IsString()) {
    return utils::pmr::string(evaluated_expr.ValueString(), utils::NewDeleteResource());
  }
  return std::nullopt;
};

TypedValue CsvRowToTypedList(csv::Reader::Row row) {
  auto *mem = row.get_allocator().GetMemoryResource();
  auto typed_columns = utils::pmr::vector<TypedValue>(mem);
  typed_columns.reserve(row.size());
  for (auto &column : row) {
    typed_columns.emplace_back(std::move(column));
  }
  return TypedValue(typed_columns, mem);
}

TypedValue CsvRowToTypedMap(csv::Reader::Row row, csv::Reader::Header header) {
  // a valid row has the same number of elements as the header
  auto *mem = row.get_allocator().GetMemoryResource();
  utils::pmr::map<utils::pmr::string, TypedValue> m(mem);
  for (auto i = 0; i < row.size(); ++i) {
    m.emplace(std::move(header[i]), std::move(row[i]));
  }
  return TypedValue(m, mem);
}

}  // namespace

class LoadCsvCursor : public Cursor {
  const LoadCsv *self_;
  const UniqueCursorPtr input_cursor_;
  bool input_is_once_;
  std::optional<csv::Reader> reader_{};

 public:
  LoadCsvCursor(const LoadCsv *self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_->input_->MakeCursor(mem)) {
    input_is_once_ = dynamic_cast<Once *>(self_->input_.get());
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("LoadCsv");

    if (MustAbort(context)) throw HintedAbortError();

    // ToDo(the-joksim):
    //  - this is an ungodly hack because the pipeline of creating a plan
    //  doesn't allow evaluating the expressions contained in self_->file_,
    //  self_->delimiter_, and self_->quote_ earlier (say, in the interpreter.cpp)
    //  without massacring the code even worse than I did here
    if (UNLIKELY(!reader_)) {
      reader_ = MakeReader(context);
    }

    bool input_pulled = input_cursor_->Pull(frame, context);

    // If the input is Once, we have to keep going until we read all the rows,
    // regardless of whether the pull on Once returned false.
    // If we have e.g. MATCH(n) LOAD CSV ... AS x SET n.name = x.name, then we
    // have to read at most cardinality(n) rows (but we can read less and stop
    // pulling MATCH).
    if (!input_is_once_ && !input_pulled) return false;

    if (auto row = reader_->GetNextRow(context.evaluation_context.memory)) {
      if (!reader_->HasHeader()) {
        frame[self_->row_var_] = CsvRowToTypedList(std::move(*row));
      } else {
        frame[self_->row_var_] = CsvRowToTypedMap(
            std::move(*row), csv::Reader::Header(reader_->GetHeader(), context.evaluation_context.memory));
      }
      return true;
    }

    return false;
  }

  void Reset() override { input_cursor_->Reset(); }
  void Shutdown() override { input_cursor_->Shutdown(); }

 private:
  csv::Reader MakeReader(ExecutionContext &context) {
    auto &eval_context = context.evaluation_context;
    Frame frame(0);
    SymbolTable symbol_table;
    auto evaluator =
        ExpressionEvaluator(&frame, symbol_table, eval_context, context.request_router, storage::v3::View::OLD);

    auto maybe_file = ToOptionalString(&evaluator, self_->file_);
    auto maybe_delim = ToOptionalString(&evaluator, self_->delimiter_);
    auto maybe_quote = ToOptionalString(&evaluator, self_->quote_);

    // No need to check if maybe_file is std::nullopt, as the parser makes sure
    // we can't get a nullptr for the 'file_' member in the LoadCsv clause.
    // Note that the reader has to be given its own memory resource, as it
    // persists between pulls, so it can't use the evalutation context memory
    // resource.
    return csv::Reader(
        *maybe_file,
        csv::Reader::Config(self_->with_header_, self_->ignore_bad_, std::move(maybe_delim), std::move(maybe_quote)),
        utils::NewDeleteResource());
  }
};

UniqueCursorPtr LoadCsv::MakeCursor(utils::MemoryResource *mem) const {
  return MakeUniqueCursorPtr<LoadCsvCursor>(mem, this, mem);
};

class ForeachCursor : public Cursor {
 public:
  explicit ForeachCursor(const Foreach &foreach, utils::MemoryResource *mem)
      : loop_variable_symbol_(foreach.loop_variable_symbol_),
        input_(foreach.input_->MakeCursor(mem)),
        updates_(foreach.update_clauses_->MakeCursor(mem)),
        expression(foreach.expression_) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP(op_name_);

    if (!input_->Pull(frame, context)) {
      return false;
    }

    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.request_router,
                                  storage::v3::View::NEW);
    TypedValue expr_result = expression->Accept(evaluator);

    if (expr_result.IsNull()) {
      return true;
    }

    if (!expr_result.IsList()) {
      throw QueryRuntimeException("FOREACH expression must resolve to a list, but got '{}'.", expr_result.type());
    }

    const auto &cache_ = expr_result.ValueList();
    for (const auto &index : cache_) {
      frame[loop_variable_symbol_] = index;
      while (updates_->Pull(frame, context)) {
      }
      ResetUpdates();
    }

    return true;
  }

  void Shutdown() override { input_->Shutdown(); }

  void ResetUpdates() { updates_->Reset(); }

  void Reset() override {
    input_->Reset();
    ResetUpdates();
  }

 private:
  const Symbol loop_variable_symbol_;
  const UniqueCursorPtr input_;
  const UniqueCursorPtr updates_;
  Expression *expression;
  const char *op_name_{"Foreach"};
};

Foreach::Foreach(std::shared_ptr<LogicalOperator> input, std::shared_ptr<LogicalOperator> updates, Expression *expr,
                 Symbol loop_variable_symbol)
    : input_(input ? std::move(input) : std::make_shared<Once>()),
      update_clauses_(std::move(updates)),
      expression_(expr),
      loop_variable_symbol_(loop_variable_symbol) {}

UniqueCursorPtr Foreach::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ForeachOperator);
  return MakeUniqueCursorPtr<ForeachCursor>(mem, *this, mem);
}

std::vector<Symbol> Foreach::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(loop_variable_symbol_);
  return symbols;
}

bool Foreach::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    input_->Accept(visitor);
    update_clauses_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

class DistributedCreateExpandCursor : public Cursor {
 public:
  using InputOperator = std::shared_ptr<memgraph::query::v2::plan::LogicalOperator>;
  DistributedCreateExpandCursor(const InputOperator &op, utils::MemoryResource *mem, const CreateExpand &self)
      : input_cursor_{op->MakeCursor(mem)}, self_{self} {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("CreateExpand");
    if (!input_cursor_->Pull(frame, context)) {
      return false;
    }
    auto &request_router = context.request_router;
    ResetExecutionState();
    {
      SCOPED_REQUEST_WAIT_PROFILE;
      request_router->CreateExpand(ExpandCreationInfoToRequest(context, frame));
    }
    return true;
  }

  bool PullMultiple(MultiFrame &multi_frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("CreateExpandMF");
    if (!input_cursor_->PullMultiple(multi_frame, context)) {
      return false;
    }
    auto request_vertices = ExpandCreationInfoToRequests(multi_frame, context);
    {
      SCOPED_REQUEST_WAIT_PROFILE;
      auto &request_router = context.request_router;
      auto results = request_router->CreateExpand(std::move(request_vertices));
      for (const auto &result : results) {
        if (result.error) {
          throw std::runtime_error("CreateExpand Request failed");
        }
      }
    }
    return true;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    ResetExecutionState();
  }

  // Get the existing node other vertex
  accessors::VertexAccessor &OtherVertex(Frame &frame) const {
    // This assumes that vertex exists
    MG_ASSERT(self_.existing_node_, "Vertex creating with edge not supported!");
    TypedValue &dest_node_value = frame[self_.node_info_.symbol];
    ExpectType(self_.node_info_.symbol, dest_node_value, TypedValue::Type::Vertex);
    return dest_node_value.ValueVertex();
  }

  std::vector<msgs::NewExpand> ExpandCreationInfoToRequest(ExecutionContext &context, Frame &frame) const {
    std::vector<msgs::NewExpand> edge_requests;
    for (const auto &edge_info : std::vector{self_.edge_info_}) {
      msgs::NewExpand request{.id = {context.edge_ids_alloc->AllocateId()}};
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, nullptr,
                                    storage::v3::View::NEW);
      request.type = {edge_info.edge_type};
      if (const auto *edge_info_properties = std::get_if<PropertiesMapList>(&edge_info.properties)) {
        for (const auto &[property, value_expression] : *edge_info_properties) {
          TypedValue val = value_expression->Accept(evaluator);
          request.properties.emplace_back(property, storage::v3::TypedValueToValue(val));
        }
      } else {
        // handle parameter
        auto property_map = evaluator.Visit(*std::get<ParameterLookup *>(edge_info.properties)).ValueMap();
        for (const auto &[property, value] : property_map) {
          const auto property_id = context.request_router->NameToProperty(std::string(property));
          request.properties.emplace_back(property_id, storage::v3::TypedValueToValue(value));
        }
      }
      // src, dest
      TypedValue &v1_value = frame[self_.input_symbol_];
      const auto &v1 = v1_value.ValueVertex();
      const auto &v2 = OtherVertex(frame);

      std::invoke([&]() {
        switch (edge_info.direction) {
          case EdgeAtom::Direction::IN: {
            request.src_vertex = v2.Id();
            request.dest_vertex = v1.Id();
            break;
          }
          case EdgeAtom::Direction::OUT: {
            request.src_vertex = v1.Id();
            request.dest_vertex = v2.Id();
            break;
          }
          case EdgeAtom::Direction::BOTH:
            LOG_FATAL("Must indicate exact expansion direction here");
        }
      });

      edge_requests.push_back(std::move(request));
    }
    return edge_requests;
  }

  std::vector<msgs::NewExpand> ExpandCreationInfoToRequests(MultiFrame &multi_frame, ExecutionContext &context) const {
    std::vector<msgs::NewExpand> edge_requests;
    auto frames_modifier = multi_frame.GetValidFramesModifier();

    for (auto &frame : frames_modifier) {
      const auto &edge_info = self_.edge_info_;
      msgs::NewExpand request{.id = {context.edge_ids_alloc->AllocateId()}};
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, nullptr,
                                    storage::v3::View::NEW);
      request.type = {edge_info.edge_type};
      if (const auto *edge_info_properties = std::get_if<PropertiesMapList>(&edge_info.properties)) {
        for (const auto &[property, value_expression] : *edge_info_properties) {
          TypedValue val = value_expression->Accept(evaluator);
          request.properties.emplace_back(property, storage::v3::TypedValueToValue(val));
        }
      } else {
        // handle parameter
        auto property_map = evaluator.Visit(*std::get<ParameterLookup *>(edge_info.properties)).ValueMap();
        for (const auto &[property, value] : property_map) {
          const auto property_id = context.request_router->NameToProperty(std::string(property));
          request.properties.emplace_back(property_id, storage::v3::TypedValueToValue(value));
        }
      }

      TypedValue &v1_value = frame[self_.input_symbol_];
      const auto &v1 = v1_value.ValueVertex();
      const auto &v2 = OtherVertex(frame);
      msgs::Edge edge{.src = request.src_vertex,
                      .dst = request.dest_vertex,
                      .properties = request.properties,
                      .id = request.id,
                      .type = request.type};
      frame[self_.edge_info_.symbol] = TypedValue(accessors::EdgeAccessor(std::move(edge), context.request_router));

      // Set src and dest vertices
      // TODO(jbajic) Currently we are only handling scenario where vertices
      // are matched
      switch (edge_info.direction) {
        case EdgeAtom::Direction::IN: {
          request.src_vertex = v2.Id();
          request.dest_vertex = v1.Id();
          break;
        }
        case EdgeAtom::Direction::OUT: {
          request.src_vertex = v1.Id();
          request.dest_vertex = v2.Id();
          break;
        }
        case EdgeAtom::Direction::BOTH:
          LOG_FATAL("Must indicate exact expansion direction here");
      }

      edge_requests.push_back(std::move(request));
    }
    return edge_requests;
  }

 private:
  void ResetExecutionState() {}

  const UniqueCursorPtr input_cursor_;
  const CreateExpand &self_;
};

class DistributedExpandCursor : public Cursor {
 public:
  DistributedExpandCursor(const Expand &self, utils::MemoryResource *mem)
      : self_(self),
        input_cursor_(self.input_->MakeCursor(mem)),
        current_in_edge_it_(current_in_edges_.begin()),
        current_out_edge_it_(current_out_edges_.begin()) {}

  using VertexAccessor = accessors::VertexAccessor;
  using EdgeAccessor = accessors::EdgeAccessor;

  static constexpr auto DirectionToMsgsDirection(const auto direction) {
    switch (direction) {
      case EdgeAtom::Direction::IN:
        return msgs::EdgeDirection::IN;
      case EdgeAtom::Direction::OUT:
        return msgs::EdgeDirection::OUT;
      case EdgeAtom::Direction::BOTH:
        return msgs::EdgeDirection::BOTH;
    }
  };

  void PullDstVertex(Frame &frame, ExecutionContext &context, const EdgeAtom::Direction direction) {
    if (self_.common_.existing_node) {
      return;
    }
    MG_ASSERT(direction != EdgeAtom::Direction::BOTH);
    const auto &edge = frame[self_.common_.edge_symbol].ValueEdge();
    static constexpr auto get_dst_vertex = [](const EdgeAccessor &edge,
                                              const EdgeAtom::Direction direction) -> accessors::VertexAccessor {
      switch (direction) {
        case EdgeAtom::Direction::IN:
          return edge.From();
        case EdgeAtom::Direction::OUT:
          return edge.To();
        case EdgeAtom::Direction::BOTH:
          throw std::runtime_error("EdgeDirection Both not implemented");
      }
    };

    frame[self_.common_.node_symbol] = get_dst_vertex(edge, direction);
  }

  bool InitEdges(Frame &frame, ExecutionContext &context) {
    // Input Vertex could be null if it is created by a failed optional match. In
    // those cases we skip that input pull and continue with the next.

    while (true) {
      if (!input_cursor_->Pull(frame, context)) return false;
      TypedValue &vertex_value = frame[self_.input_symbol_];

      // Null check due to possible failed optional match.
      if (vertex_value.IsNull()) continue;

      ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
      auto &vertex = vertex_value.ValueVertex();
      msgs::ExpandOneRequest request;
      request.direction = DirectionToMsgsDirection(self_.common_.direction);
      std::transform(self_.common_.edge_types.begin(), self_.common_.edge_types.end(),
                     std::back_inserter(request.edge_types),
                     [](const storage::v3::EdgeTypeId edge_type_id) { return msgs::EdgeType{edge_type_id}; });
      // to not fetch any properties of the edges
      request.edge_properties.emplace();
      request.src_vertices.push_back(vertex.Id());
      request.edge_properties.emplace();
      request.src_vertex_properties.emplace();
      auto result_rows = std::invoke([&context, &request]() mutable {
        SCOPED_REQUEST_WAIT_PROFILE;
        return context.request_router->ExpandOne(std::move(request));
      });
      MG_ASSERT(result_rows.size() == 1);
      auto &result_row = result_rows.front();

      if (self_.common_.existing_node) {
        const auto &node = frame[self_.common_.node_symbol].ValueVertex().Id();
        auto &in = result_row.in_edges_with_specific_properties;
        std::erase_if(in, [&node](auto &edge) { return edge.other_end != node; });
        auto &out = result_row.out_edges_with_specific_properties;
        std::erase_if(out, [&node](auto &edge) { return edge.other_end != node; });
      }

      const auto convert_edges = [&vertex, &context](
                                     std::vector<msgs::ExpandOneResultRow::EdgeWithSpecificProperties> &&edge_messages,
                                     const EdgeAtom::Direction direction) {
        std::vector<EdgeAccessor> edge_accessors;
        edge_accessors.reserve(edge_messages.size());

        switch (direction) {
          case EdgeAtom::Direction::IN: {
            for (auto &edge : edge_messages) {
              edge_accessors.emplace_back(msgs::Edge{std::move(edge.other_end), vertex.Id(), {}, {edge.gid}, edge.type},
                                          context.request_router);
            }
            break;
          }
          case EdgeAtom::Direction::OUT: {
            for (auto &edge : edge_messages) {
              edge_accessors.emplace_back(msgs::Edge{vertex.Id(), std::move(edge.other_end), {}, {edge.gid}, edge.type},
                                          context.request_router);
            }
            break;
          }
          case EdgeAtom::Direction::BOTH: {
            LOG_FATAL("Must indicate exact expansion direction here");
          }
        }
        return edge_accessors;
      };

      current_in_edges_ =
          convert_edges(std::move(result_row.in_edges_with_specific_properties), EdgeAtom::Direction::IN);
      current_in_edge_it_ = current_in_edges_.begin();
      current_out_edges_ =
          convert_edges(std::move(result_row.out_edges_with_specific_properties), EdgeAtom::Direction::OUT);
      current_out_edge_it_ = current_out_edges_.begin();
      return true;
    }
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("DistributedExpand");
    // A helper function for expanding a node from an edge.

    while (true) {
      if (MustAbort(context)) throw HintedAbortError();
      // attempt to get a value from the incoming edges
      if (current_in_edge_it_ != current_in_edges_.end()) {
        auto &edge = *current_in_edge_it_;
        ++current_in_edge_it_;
        frame[self_.common_.edge_symbol] = edge;
        PullDstVertex(frame, context, EdgeAtom::Direction::IN);
        return true;
      }

      // attempt to get a value from the outgoing edges
      if (current_out_edge_it_ != current_out_edges_.end()) {
        auto &edge = *current_out_edge_it_;
        ++current_out_edge_it_;
        if (self_.common_.direction == EdgeAtom::Direction::BOTH && edge.IsCycle()) {
          continue;
        };
        frame[self_.common_.edge_symbol] = edge;
        PullDstVertex(frame, context, EdgeAtom::Direction::OUT);
        return true;
      }

      // If we are here, either the edges have not been initialized,
      // or they have been exhausted. Attempt to initialize the edges.
      if (!InitEdges(frame, context)) return false;

      // we have re-initialized the edges, continue with the loop
    }
  }

  void InitEdgesMultiple() {
    // This function won't work if any vertex id is duplicated in the input, because:
    //  1. vertex_id_to_result_row is not a multimap
    //  2. if self_.common_.existing_node is true, then we erase edges that might be necessary for the input
    //  vertex on a
    //     later frame
    const auto &frame = (*own_frames_it_);
    const auto &vertex_value = frame[self_.input_symbol_];

    if (vertex_value.IsNull()) {
      ResetMultiFrameEdgeIts();
      return;
    }

    ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
    const auto &vertex = vertex_value.ValueVertex();

    current_vertex_ = &vertex;

    auto &ref_counted_result_row = vertex_id_to_result_row.at(vertex.Id());
    auto &result_row = *ref_counted_result_row.result_row;

    current_in_edge_mf_it_ = result_row.in_edges_with_specific_properties.begin();
    in_edges_end_it_ = result_row.in_edges_with_specific_properties.end();
    AdvanceUntilSuitableEdge(current_in_edge_mf_it_, in_edges_end_it_);
    current_out_edge_mf_it_ = result_row.out_edges_with_specific_properties.begin();
    out_edges_end_it_ = result_row.out_edges_with_specific_properties.end();
    AdvanceUntilSuitableEdge(current_out_edge_mf_it_, out_edges_end_it_);

    if (ref_counted_result_row.ref_count == 1) {
      vertex_id_to_result_row.erase(vertex.Id());
    } else {
      ref_counted_result_row.ref_count--;
    }
  }

  bool PullInputFrames(ExecutionContext &context) {
    const auto pulled_any = input_cursor_->PullMultiple(*own_multi_frame_, context);
    // These needs to be updated regardless of the result of the pull, otherwise the consumer and iterator might
    // get corrupted because of the operations done on our MultiFrame.
    own_frames_consumer_ = own_multi_frame_->GetValidFramesConsumer();
    own_frames_it_ = own_frames_consumer_->begin();
    if (!pulled_any) {
      return false;
    }

    vertex_id_to_result_row.clear();

    msgs::ExpandOneRequest request;
    request.direction = DirectionToMsgsDirection(self_.common_.direction);
    std::transform(self_.common_.edge_types.begin(), self_.common_.edge_types.end(),
                   std::back_inserter(request.edge_types),
                   [](const storage::v3::EdgeTypeId edge_type_id) { return msgs::EdgeType{edge_type_id}; });
    // to not fetch any properties of the edges
    request.edge_properties.emplace();
    request.src_vertex_properties.emplace();
    for (const auto &frame : own_multi_frame_->GetValidFramesReader()) {
      const auto &vertex_value = frame[self_.input_symbol_];

      // Null check due to possible failed optional match.
      MG_ASSERT(!vertex_value.IsNull());

      ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
      const auto &vertex = vertex_value.ValueVertex();
      auto [it, inserted] = vertex_id_to_result_row.try_emplace(vertex.Id(), RefCountedResultRow{1U, nullptr});

      if (inserted) {
        request.src_vertices.push_back(vertex.Id());
      } else {
        it->second.ref_count++;
      }
    }

    result_rows_ = std::invoke([&context, &request]() mutable {
      SCOPED_REQUEST_WAIT_PROFILE;
      return context.request_router->ExpandOne(std::move(request));
    });
    for (auto &row : result_rows_) {
      vertex_id_to_result_row[row.src_vertex.id].result_row = &row;
    }

    return true;
  }

  bool PullMultiple(MultiFrame &output_multi_frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("DistributedExpandMF");
    EnsureOwnMultiFrameIsGood(output_multi_frame);
    // A helper function for expanding a node from an edge.

    auto output_frames_populator = output_multi_frame.GetInvalidFramesPopulator();
    auto populated_any = false;

    while (true) {
      switch (state_) {
        case State::PullInputAndEdges: {
          if (!PullInputFrames(context)) {
            state_ = State::Exhausted;
            return populated_any;
          }
          state_ = State::InitInOutEdgesIt;
          break;
        }
        case State::InitInOutEdgesIt: {
          if (own_frames_it_ == own_frames_consumer_->end()) {
            state_ = State::PullInputAndEdges;
          } else {
            InitEdgesMultiple();
            state_ = State::PopulateOutput;
          }
          break;
        }
        case State::PopulateOutput: {
          if (!output_multi_frame.HasInvalidFrame()) {
            return populated_any;
          }
          if (current_in_edge_mf_it_ == in_edges_end_it_ && current_out_edge_mf_it_ == out_edges_end_it_) {
            own_frames_it_->MakeInvalid();
            ++own_frames_it_;
            state_ = State::InitInOutEdgesIt;
            continue;
          }
          auto populate_edges = [this, &context, &output_frames_populator, &populated_any](
                                    const EdgeAtom::Direction direction, EdgesIterator &current,
                                    const EdgesIterator &end) {
            for (auto output_frame_it = output_frames_populator.begin();
                 output_frame_it != output_frames_populator.end() && current != end; ++output_frame_it) {
              auto &edge = *current;
              auto &output_frame = *output_frame_it;
              output_frame = *own_frames_it_;
              switch (direction) {
                case EdgeAtom::Direction::IN: {
                  output_frame[self_.common_.edge_symbol] =
                      EdgeAccessor{msgs::Edge{edge.other_end, current_vertex_->Id(), {}, {edge.gid}, edge.type},
                                   context.request_router};
                  break;
                }
                case EdgeAtom::Direction::OUT: {
                  output_frame[self_.common_.edge_symbol] =
                      EdgeAccessor{msgs::Edge{current_vertex_->Id(), edge.other_end, {}, {edge.gid}, edge.type},
                                   context.request_router};
                  break;
                }
                case EdgeAtom::Direction::BOTH: {
                  LOG_FATAL("Must indicate exact expansion direction here");
                }
              };
              PullDstVertex(output_frame, context, direction);
              ++current;
              AdvanceUntilSuitableEdge(current, end);
              populated_any = true;
            }
          };
          populate_edges(EdgeAtom::Direction::IN, current_in_edge_mf_it_, in_edges_end_it_);
          populate_edges(EdgeAtom::Direction::OUT, current_out_edge_mf_it_, out_edges_end_it_);
          break;
        }
        case State::Exhausted: {
          return populated_any;
        }
      }
    }
    return populated_any;
  }

  void PushDown(const MultiFrame &multi_frame) override { input_cursor_->PushDown(multi_frame); }

  void EnsureOwnMultiFrameIsGood(MultiFrame &output_multi_frame) {
    if (!own_multi_frame_.has_value()) {
      own_multi_frame_.emplace(MultiFrame(output_multi_frame.GetFirstFrame().Elems().size(),
                                          FLAGS_default_multi_frame_size, output_multi_frame.GetMemoryResource()));
      own_frames_consumer_.emplace(own_multi_frame_->GetValidFramesConsumer());
      own_frames_it_ = own_frames_consumer_->begin();
    }
    MG_ASSERT(output_multi_frame.GetFirstFrame().Elems().size() == own_multi_frame_->GetFirstFrame().Elems().size());
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    vertex_id_to_result_row.clear();
    result_rows_.clear();
    own_frames_it_ = ValidFramesConsumer::Iterator{};
    own_frames_consumer_.reset();
    if (own_multi_frame_.has_value()) {
      own_multi_frame_->MakeAllFramesInvalid();
    }
    state_ = State::PullInputAndEdges;

    current_in_edges_.clear();
    current_out_edges_.clear();
    current_in_edge_it_ = current_in_edges_.end();
    current_out_edge_it_ = current_out_edges_.end();

    ResetMultiFrameEdgeIts();
  }

 private:
  enum class State { PullInputAndEdges, InitInOutEdgesIt, PopulateOutput, Exhausted };

  struct RefCountedResultRow {
    size_t ref_count{0U};
    msgs::ExpandOneResultRow *result_row{nullptr};
  };

  using EdgeWithSpecificProperties = msgs::ExpandOneResultRow::EdgeWithSpecificProperties;
  using EdgesVector = std::vector<EdgeWithSpecificProperties>;
  using EdgesIterator = EdgesVector::iterator;

  void ResetMultiFrameEdgeIts() {
    in_edges_end_it_ = EdgesIterator{};
    current_in_edge_mf_it_ = in_edges_end_it_;
    out_edges_end_it_ = EdgesIterator{};
    current_out_edge_mf_it_ = out_edges_end_it_;
  }

  void AdvanceUntilSuitableEdge(EdgesIterator &current, const EdgesIterator &end) {
    if (!self_.common_.existing_node) {
      return;
    }

    const auto &existing_node_value = (*own_frames_it_)[self_.common_.node_symbol];
    if (existing_node_value.IsNull()) {
      current = end;
      return;
    }
    const auto &existing_node = existing_node_value.ValueVertex();
    current = std::find_if(current, end, [&existing_node](const EdgeWithSpecificProperties &edge) {
      return edge.other_end == existing_node.Id();
    });
  }

  const Expand &self_;
  const UniqueCursorPtr input_cursor_;
  EdgesIterator current_in_edge_mf_it_;
  EdgesIterator in_edges_end_it_;
  EdgesIterator current_out_edge_mf_it_;
  EdgesIterator out_edges_end_it_;
  State state_{State::PullInputAndEdges};
  std::optional<MultiFrame> own_multi_frame_;
  std::optional<ValidFramesConsumer> own_frames_consumer_;
  const VertexAccessor *current_vertex_{nullptr};
  ValidFramesConsumer::Iterator own_frames_it_;
  std::vector<msgs::ExpandOneResultRow> result_rows_;
  // This won't work if any vertex id is duplicated in the input
  std::unordered_map<msgs::VertexId, RefCountedResultRow> vertex_id_to_result_row;

  // TODO(antaljanosbenjamin): Remove when single frame approach is removed
  std::vector<EdgeAccessor> current_in_edges_;
  std::vector<EdgeAccessor> current_out_edges_;
  std::vector<EdgeAccessor>::iterator current_in_edge_it_;
  std::vector<EdgeAccessor>::iterator current_out_edge_it_;
};

}  // namespace memgraph::query::v2::plan
