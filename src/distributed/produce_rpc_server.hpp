/// @file
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "database/distributed/graph_db.hpp"
#include "database/distributed/graph_db_accessor.hpp"
#include "distributed/coordination.hpp"
#include "distributed/plan_consumer.hpp"
#include "distributed/pull_produce_rpc_messages.hpp"
#include "query/context.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/interpret/frame.hpp"
#include "query/plan/operator.hpp"
#include "query/typed_value.hpp"
#include "transactions/type.hpp"

namespace database {
class Worker;
}

namespace tx {
class EngineWorker;
}

namespace distributed {

class DataManager;

/// Handles the execution of a plan on the worker, requested by the remote
/// master. Assumes that (tx id, command id, plan id) uniquely identifies an
/// execution, and that there will never be parallel requests for the same
/// execution thus  identified.
class ProduceRpcServer {
  /// Encapsulates a Cursor execution in progress. Can be used for pulling a
  /// single result from the execution, or pulling all and accumulating the
  /// results. Accumulations are used for synchronizing updates in distributed
  /// MG (see query::plan::Synchronize).
  class OngoingProduce {
   public:
    OngoingProduce(database::Worker *db, tx::TransactionId tx_id,
                   const PlanConsumer::PlanPack &plan_pack, int64_t timestamp,
                   const query::Parameters &parameters,
                   std::vector<query::Symbol> pull_symbols);

    /// Returns a vector of typed values (one for each `pull_symbol`), and an
    /// indication of the pull result. The result data is valid only if the
    /// returned state is CURSOR_IN_PROGRESS.
    std::pair<std::vector<query::TypedValue>, PullState> Pull();

    /// Accumulates all the frames pulled from the cursor and returns
    /// CURSOR_EXHAUSTED. If an error occurs, an appropriate value is returned.
    PullState Accumulate();

    void Reset();

   private:
    std::unique_ptr<database::GraphDbAccessor> dba_;
    query::ExecutionContext context_;
    std::vector<query::Symbol> pull_symbols_;
    query::Frame frame_;
    PullState cursor_state_{PullState::CURSOR_IN_PROGRESS};
    std::vector<std::vector<query::TypedValue>> accumulation_;
    // execution_memory_ is unique_ptr because we are passing the address to
    // cursor_, and we want to preserve the pointer in case we get moved.
    std::unique_ptr<utils::MonotonicBufferResource> execution_memory_;
    query::plan::UniqueCursorPtr cursor_;

    /// Pulls and returns a single result from the cursor.
    std::pair<std::vector<query::TypedValue>, PullState> PullOneFromCursor();
  };

 public:
  ProduceRpcServer(database::Worker *db, tx::EngineWorker *tx_engine,
                   distributed::Coordination *coordination,
                   const PlanConsumer &plan_consumer,
                   DataManager *data_manager);

  /// Clears all ongoing produces that are older than the oldest active
  /// transaction. This function should be registered in the transaction engine
  /// for transactional cache cleanup.
  void ClearTransactionalCache(tx::TransactionId oldest_active);

 private:
  std::mutex ongoing_produces_lock_;
  /// Mapping of (tx id, command id, plan id) to OngoingProduce.
  /// The command_id should be the command_id at the initialization of a cursor
  /// that can call ProduceRpcServer.
  std::map<std::tuple<tx::TransactionId, tx::CommandId, int64_t>,
           OngoingProduce>
      ongoing_produces_;
  database::Worker *db_;
  const distributed::PlanConsumer &plan_consumer_;
  tx::EngineWorker *tx_engine_;

  /// Gets an ongoing produce for the given pull request. Creates a new one if
  /// there is none currently existing.
  OngoingProduce &GetOngoingProduce(const PullReq &req);

  /// Performs a single remote pull for the given request.
  PullResData Pull(const PullReq &req);

  /// Resets the cursor for an ongoing produce.
  void Reset(const ResetCursorReq &req);
};

}  // namespace distributed