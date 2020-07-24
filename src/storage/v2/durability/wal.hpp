#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

#include "storage/v2/config.hpp"
#include "storage/v2/delta.hpp"
#include "storage/v2/durability/metadata.hpp"
#include "storage/v2/durability/serialization.hpp"
#include "storage/v2/edge.hpp"
#include "storage/v2/id_types.hpp"
#include "storage/v2/name_id_mapper.hpp"
#include "storage/v2/property_value.hpp"
#include "storage/v2/vertex.hpp"
#include "utils/skip_list.hpp"

namespace storage::durability {

/// Structure used to hold information about a WAL.
struct WalInfo {
  uint64_t offset_metadata;
  uint64_t offset_deltas;

  std::string uuid;
  uint64_t seq_num;
  uint64_t from_timestamp;
  uint64_t to_timestamp;
  uint64_t num_deltas;
};

/// Structure used to return loaded WAL delta data.
struct WalDeltaData {
  enum class Type {
    VERTEX_CREATE,
    VERTEX_DELETE,
    VERTEX_ADD_LABEL,
    VERTEX_REMOVE_LABEL,
    VERTEX_SET_PROPERTY,
    EDGE_CREATE,
    EDGE_DELETE,
    EDGE_SET_PROPERTY,
    TRANSACTION_END,
    LABEL_INDEX_CREATE,
    LABEL_INDEX_DROP,
    LABEL_PROPERTY_INDEX_CREATE,
    LABEL_PROPERTY_INDEX_DROP,
    EXISTENCE_CONSTRAINT_CREATE,
    EXISTENCE_CONSTRAINT_DROP,
    UNIQUE_CONSTRAINT_CREATE,
    UNIQUE_CONSTRAINT_DROP,
  };

  Type type{Type::TRANSACTION_END};

  struct {
    Gid gid;
  } vertex_create_delete;

  struct {
    Gid gid;
    std::string label;
  } vertex_add_remove_label;

  struct {
    Gid gid;
    std::string property;
    PropertyValue value;
  } vertex_edge_set_property;

  struct {
    Gid gid;
    std::string edge_type;
    Gid from_vertex;
    Gid to_vertex;
  } edge_create_delete;

  struct {
    std::string label;
  } operation_label;

  struct {
    std::string label;
    std::string property;
  } operation_label_property;

  struct {
    std::string label;
    std::set<std::string> properties;
  } operation_label_properties;
};

bool operator==(const WalDeltaData &a, const WalDeltaData &b);
bool operator!=(const WalDeltaData &a, const WalDeltaData &b);

/// Enum used to indicate a global database operation that isn't transactional.
enum class StorageGlobalOperation {
  LABEL_INDEX_CREATE,
  LABEL_INDEX_DROP,
  LABEL_PROPERTY_INDEX_CREATE,
  LABEL_PROPERTY_INDEX_DROP,
  EXISTENCE_CONSTRAINT_CREATE,
  EXISTENCE_CONSTRAINT_DROP,
  UNIQUE_CONSTRAINT_CREATE,
  UNIQUE_CONSTRAINT_DROP,
};

/// Function used to read information about the WAL file.
/// @throw RecoveryFailure
WalInfo ReadWalInfo(const std::filesystem::path &path);

/// Function used to read the WAL delta header. The function returns the delta
/// timestamp.
/// @throw RecoveryFailure
uint64_t ReadWalDeltaHeader(Decoder *wal);

/// Function used to read the current WAL delta data. The function returns the
/// read delta data. The WAL delta header must be read before calling this
/// function.
/// @throw RecoveryFailure
WalDeltaData ReadWalDeltaData(Decoder *wal);

/// Function used to skip the current WAL delta data. The function returns the
/// skipped delta type. The WAL delta header must be read before calling this
/// function.
/// @throw RecoveryFailure
WalDeltaData::Type SkipWalDeltaData(Decoder *wal);

/// Function used to load the WAL data into the storage.
/// @throw RecoveryFailure
RecoveryInfo LoadWal(const std::filesystem::path &path,
                     RecoveredIndicesAndConstraints *indices_constraints,
                     std::optional<uint64_t> snapshot_timestamp,
                     utils::SkipList<Vertex> *vertices,
                     utils::SkipList<Edge> *edges, NameIdMapper *name_id_mapper,
                     std::atomic<uint64_t> *edge_count, Config::Items items);

/// WalFile class used to append deltas and operations to the WAL file.
class WalFile {
 public:
  WalFile(const std::filesystem::path &wal_directory, const std::string &uuid,
          Config::Items items, NameIdMapper *name_id_mapper, uint64_t seq_num);

  WalFile(const WalFile &) = delete;
  WalFile(WalFile &&) = delete;
  WalFile &operator=(const WalFile &) = delete;
  WalFile &operator=(WalFile &&) = delete;

  ~WalFile();

  void AppendDelta(const Delta &delta, const Vertex &vertex,
                   uint64_t timestamp);
  void AppendDelta(const Delta &delta, const Edge &edge, uint64_t timestamp);

  void AppendTransactionEnd(uint64_t timestamp);

  void AppendOperation(StorageGlobalOperation operation, LabelId label,
                       const std::set<PropertyId> &properties,
                       uint64_t timestamp);

  void Sync();

  uint64_t GetSize();

 private:
  void UpdateStats(uint64_t timestamp);

  Config::Items items_;
  NameIdMapper *name_id_mapper_;
  Encoder wal_;
  std::filesystem::path path_;
  uint64_t from_timestamp_;
  uint64_t to_timestamp_;
  uint64_t count_;
};

}  // namespace storage::durability