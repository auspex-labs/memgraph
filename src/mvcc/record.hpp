#pragma once

#include <atomic>
#include <iostream>

#include "transactions/commit_log.hpp"
#include "transactions/engine.hpp"
#include "transactions/transaction.hpp"

#include "mvcc/version.hpp"
#include "storage/locking/record_lock.hpp"

// the mvcc implementation used here is very much like postgresql's
// more info: https://momjian.us/main/writings/pgsql/mvcc.pdf

namespace mvcc {

template <class T>
class Record : public Version<T> {
 public:
  Record() = default;
  Record(const Record &) = delete;
  Record &operator=(const Record &) = delete;
  Record(Record &&) = delete;
  Record &operator=(Record &&) = delete;

  // check if this record is visible to the transaction t
  bool visible(const tx::Transaction &t) {
    // Mike Olson says 17 march 1993: the tests in this routine are correct;
    // if you think they're not, you're wrong, and you should think about it
    // again. i know, it happened to me.

    // fetch expiration info in a safe way (see fetch_exp for details)
    tx::transaction_id_t tx_exp;
    tx::command_id_t cmd_exp;
    std::tie(tx_exp, cmd_exp) = fetch_exp();

    return (
        (tx_.cre == t.id_ &&      // inserted by the current transaction
         cmd_.cre < t.cid() &&    // before this command, and
         (tx_exp == 0 ||          // the row has not been deleted, or
          (tx_exp == t.id_ &&     // it was deleted by the current
                                  // transaction
           cmd_exp >= t.cid())))  // but not before this command,
        ||                        // or
        (committed(Hints::kCre, tx_.cre, t) &&  // the record was inserted by a
                                                // committed transaction, and
         (tx_exp == 0 ||           // the record has not been deleted, or
          (tx_exp == t.id_ &&      // the row is being deleted by this
                                   // transaction
           cmd_exp >= t.cid()) ||  // but it's not deleted "yet", or
          (tx_exp != t.id_ &&      // the row was deleted by another
                                   // transaction
           !committed(Hints::kExp, tx_exp, t)  // that has not been committed
           ))));
  }

  void mark_created(const tx::Transaction &t) {
    debug_assert(tx_.cre == 0, "Marking node as created twice.");
    tx_.cre = t.id_;
    cmd_.cre = t.cid();
  }

  void mark_expired(const tx::Transaction &t) {
    if (tx_.exp != 0) hints_.Clear(Hints::kExp);
    tx_.exp = t.id_;
    cmd_.exp = t.cid();
  }

  bool exp_committed(tx::Engine &engine) {
    return committed(Hints::kExp, tx_.exp, engine);
  }

  /**
   * Check if this record is visible w.r.t. to the given garbage collection
   * snapshot. See source comments for exact logic.
   *
   * @param snapshot - the GC snapshot. Consists of the oldest active
   * transaction's snapshot, with that transaction's id appened as last.
   */
  bool is_not_visible_from(const tx::Snapshot &snapshot,
                           const tx::Engine &engine) const {
    // first get tx.exp so that all the subsequent checks operate on
    // the same id. otherwise there could be a race condition
    auto exp_id = tx_.exp.load();

    // a record is NOT visible if:
    // 1. it creating transaction aborted (last check)
    // OR
    // 2. a) it's expiration is not 0 (some transaction expired it)
    //    AND
    //    b) the expiring transaction is older than latest active
    //    AND
    //    c) that transaction committed (as opposed to aborted)
    //    AND
    //    d) that transaction is not in oldest active transaction's
    //       snapshot (consequently also not in the snapshots of
    //       newer transactions)
    return (exp_id != 0 && exp_id < snapshot.back() &&
            committed(Hints::kExp, exp_id, engine) &&
            !snapshot.contains(exp_id)) ||
           cre_aborted(engine);
  }

  // TODO: Test this
  // True if this record is visible for write.
  // Note that this logic is different from the one above
  // in the sense that a record is visible if created before
  // OR DURING this command. this is done to support cypher's
  // queries which can match, update and return in the same query
  bool is_visible_write(const tx::Transaction &t) {
    // fetch expiration info in a safe way (see fetch_exp for details)
    tx::transaction_id_t tx_exp;
    tx::command_id_t cmd_exp;
    std::tie(tx_exp, cmd_exp) = fetch_exp();

    return (tx_.cre == t.id_ &&       // inserted by the current transaction
            cmd_.cre <= t.cid() &&    // before OR DURING this command, and
            (tx_exp == 0 ||           // the row has not been deleted, or
             (tx_exp == t.id_ &&      // it was deleted by the current
                                      // transaction
              cmd_exp >= t.cid())));  // but not before this command,
  }

  /**
   * True if this record is created in the current command
   * of the given transaction.
   */
  bool is_created_by(const tx::Transaction &t) {
    return tx_.cre == t.id_ && cmd_.cre == t.cid();
  }

  /**
   * True if this record is expired in the current command
   * of the given transaction.
   */
  bool is_expired_by(const tx::Transaction &t) {
    return tx_.exp == t.id_ && cmd_.exp == t.cid();
  }

  const auto &tx() const { return tx_; }
  const auto &cmd() const { return cmd_; }

 private:
  /**
   * Fast indicators if a transaction has committed or aborted. It is possible
   * the hints do not have that information, in which case the commit log needs
   * to be consulted (a slower operation).
   */
  class Hints {
   public:
    /// Masks for the creation/expration and commit/abort positions.
    static constexpr uint8_t kCre = 0b0011;
    static constexpr uint8_t kExp = 0b1100;
    static constexpr uint8_t kCmt = 0b0101;
    static constexpr uint8_t kAbt = 0b1010;

    /** Retruns true if any bit under the given mask is set. */
    bool Get(uint8_t mask) const { return bits_ & mask; }

    /** Sets all the bits under the given mask. */
    void Set(uint8_t mask) { bits_.fetch_or(mask); }

    /** Clears all the bits under the given mask. */
    void Clear(uint8_t mask) { bits_.fetch_and(~mask); }

   private:
    std::atomic<uint8_t> bits_{0};
  };

  template <typename TId>
  struct CreExp {
    std::atomic<TId> cre{0};
    std::atomic<TId> exp{0};
  };

  // tx.cre is the id of the transaction that created the record
  // and tx.exp is the id of the transaction that deleted the record
  // These values are used to determine the visibility of the record
  // to the current transaction.
  CreExp<tx::transaction_id_t> tx_;

  // cmd.cre is the id of the command in this transaction that created the
  // record and cmd.exp is the id of the command in this transaction that
  // deleted the record. These values are used to determine the visibility
  // of the record to the current command in the running transaction.
  CreExp<tx::command_id_t> cmd_;

  mutable Hints hints_;
  /** Fetch the (transaction, command) expiration before the check
   * because they can be concurrently modified by multiple transactions.
   * Do it in a loop to ensure that command is consistent with transaction.
   */
  auto fetch_exp() {
    tx::transaction_id_t tx_exp;
    tx::command_id_t cmd_exp;
    do {
      tx_exp = tx_.exp;
      cmd_exp = cmd_.exp;
    } while (tx_exp != tx_.exp);
    return std::make_pair(tx_exp, cmd_exp);
  }

  /**
   * @brief - Check if the transaction with the given `id`
   * is commited from the perspective of transaction `t`.
   *
   * Evaluates to true if that transaction has committed,
   * it started before `t` and it's not in it's snapshot.
   *
   * about transactions commit/abort status
   * @param mask - Hint bits mask (either Hints::kCre or Hints::kExp).
   * @param id - id to check if it's commited and visible
   * @return true if the id is commited and visible for the transaction t.
   */
  // TODO: Rename this function. Its semantics is different than of function
  // below, but it has a same name.
  bool committed(uint8_t mask, tx::transaction_id_t id,
                 const tx::Transaction &t) {
    debug_assert(mask == Hints::kCre || mask == Hints::kExp,
                 "Mask must be either kCre or kExp");
    // Dominik Gleich says 4 april 2017: the tests in this routine are correct;
    // if you think they're not, you're wrong, and you should think about it
    // again. I know, it happened to me (and also to Matej Gradicek).

    // You certainly can't see the transaction with id greater than yours as
    // that means it started after this transaction and if it commited, it
    // commited after this transaction has started.
    if (id >= t.id_) return false;

    // The creating transaction is still in progress (examine snapshot)
    if (t.snapshot().contains(id)) return false;

    return committed(mask, id, t.engine_);
  }

  /**
   * @brief - Check if the transaction with the given `id` is committed.
   *
   * @param mask - Hint bits mask (either Hints::kCre or Hints::kExp).
   * @param id - id to check if commited
   * @param engine - engine instance with information about transaction
   * statuses
   * @return true if it's commited, false otherwise
   */
  bool committed(uint8_t mask, tx::transaction_id_t id,
                 const tx::Engine &engine) const {
    debug_assert(mask == Hints::kCre || mask == Hints::kExp,
                 "Mask must be either kCre or kExp");
    // If hints are set, return if id is committed.
    if (hints_.Get(mask)) return hints_.Get(Hints::kCmt & mask);

    // If hints are not set consult the commit log.
    auto info = engine.clog().fetch_info(id);
    if (info.is_committed()) {
      hints_.Set(Hints::kCmt & mask);
      return true;
    }
    if (info.is_aborted() && mask == Hints::kCre) {
      // We can't set hints for aborted if mask is kExp because of a
      // race-condition that can occurr when tx.exp gets changed by some
      // transaction.
      //
      // This is not a problem with hints.cre.X because only one transaction
      // ever creates a record
      hints_.Set(Hints::kAbt & mask);
    }

    return false;
  }

  /**
   * @brief - Check if tx_.cre is aborted. If you need to check for exp
   * transaction do it manually by looking at commit log. This function can't do
   * that for you since hints can't be used for exp transaction (reason is
   * described in function above).
   *
   * @param engine - engine instance with information about transaction
   * statuses
   * @return true if it's aborted, false otherwise
   */
  bool cre_aborted(const tx::Engine &engine) const {
    // If hints are set, return if id is committed.
    if (hints_.Get(Hints::kCre)) return hints_.Get(Hints::kAbt & Hints::kCre);

    // If hints are not set consult the commit log.
    auto info = engine.clog().fetch_info(tx_.cre);
    if (info.is_aborted()) {
      hints_.Set(Hints::kAbt & Hints::kCre);
      return true;
    }
    if (info.is_committed()) {
      hints_.Set(Hints::kCmt & Hints::kCre);
    }
    return false;
  }
};
}
