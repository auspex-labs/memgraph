#pragma once

#include <glog/logging.h>

#include "data_structures/concurrent/skiplist.hpp"
#include "mvcc/version_list.hpp"
#include "storage/deferred_deleter.hpp"
#include "storage/deferred_deleter.hpp"
#include "transactions/engine.hpp"

/**
 * @brief - Garbage collects deleted records.
 * @Tparam T type of underlying record in mvcc
 */
template <typename T>
class GarbageCollector {
 public:
  GarbageCollector(SkipList<mvcc::VersionList<T> *> &skiplist,
                   DeferredDeleter<T> &record_deleter,
                   DeferredDeleter<mvcc::VersionList<T>> &version_list_deleter)
      : skiplist_(skiplist),
        record_deleter_(record_deleter),
        version_list_deleter_(version_list_deleter) {}

  /**
   * @brief - Runs garbage collector. Populates deferred deleters with version
   * lists and records.
   *
   * @param snapshot - the GC snapshot. Consists of the oldest active
   * transaction's snapshot, with that transaction's id appened as last.
   * @param engine - reference to engine object
   */
  void Run(const tx::Snapshot &snapshot, const tx::Engine &engine) {
    auto collection_accessor = this->skiplist_.access();
    uint64_t count = 0;
    std::vector<typename DeferredDeleter<T>::DeletedObject> deleted_records;
    std::vector<typename DeferredDeleter<mvcc::VersionList<T>>::DeletedObject>
        deleted_version_lists;
    for (auto version_list : collection_accessor) {
      // If the version_list is empty, i.e. there is nothing else to be read
      // from it we can delete it.
      auto ret = version_list->GcDeleted(snapshot, engine);
      if (ret.first) {
        deleted_version_lists.emplace_back(version_list,
                                           engine.LockFreeCount());
        count += collection_accessor.remove(version_list);
      }
      if (ret.second != nullptr)
        deleted_records.emplace_back(ret.second, engine.LockFreeCount());
    }
    DLOG_IF(INFO, count > 0) << "GC started cleaning with snapshot: "
                             << snapshot;
    DLOG_IF(INFO, count > 0) << "Destroyed: " << count;

    // Add records to deleter, with the id larger or equal than the last active
    // transaction.
    record_deleter_.AddObjects(deleted_records);
    // Add version_lists to deleter, with the id larger or equal than the last
    // active transaction.
    version_list_deleter_.AddObjects(deleted_version_lists);
  }

 private:
  SkipList<mvcc::VersionList<T> *> &skiplist_;
  DeferredDeleter<T> &record_deleter_;
  DeferredDeleter<mvcc::VersionList<T>> &version_list_deleter_;
};
