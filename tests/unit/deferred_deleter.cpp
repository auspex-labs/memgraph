#include "gtest/gtest.h"

#include "mvcc/record.hpp"
#include "storage/deferred_deleter.hpp"
#include "storage/vertex.hpp"

#include "mvcc_gc_common.hpp"

// Add and count objects.
TEST(DeferredDeleter, AddObjects) {
  DeferredDeleter<Vertex> deleter;
  for (int i = 0; i < 10; ++i) {
    std::vector<DeferredDeleter<Vertex>::DeletedObject> v;
    v.emplace_back(new Vertex, 5);
    v.emplace_back(new Vertex, 5);
    deleter.AddObjects(v);
    EXPECT_EQ(deleter.Count(), (i + 1) * 2);
  }
  deleter.FreeExpiredObjects(tx::Transaction::MaxId());
}

// Check that the deleter can't be destroyed while it still has objects.
TEST(DeferredDeleter, Destructor) {
  std::atomic<int> count{0};
  DeferredDeleter<DestrCountRec> *deleter = new DeferredDeleter<DestrCountRec>;
  for (int i = 0; i < 10; ++i) {
    std::vector<DeferredDeleter<DestrCountRec>::DeletedObject> v;
    v.emplace_back(new DestrCountRec(count), 5);
    v.emplace_back(new DestrCountRec(count), 5);
    deleter->AddObjects(v);
    EXPECT_EQ(deleter->Count(), (i + 1) * 2);
  }
  EXPECT_EQ(0, count);
  EXPECT_DEATH(delete deleter, "");
  // We shouldn't leak memory.
  deleter->FreeExpiredObjects(tx::Transaction::MaxId());
  delete deleter;
}

// Check if deleter frees objects.
TEST(DeferredDeleter, FreeExpiredObjects) {
  DeferredDeleter<DestrCountRec> deleter;
  std::vector<DeferredDeleter<DestrCountRec>::DeletedObject> v;
  std::atomic<int> count{0};
  v.emplace_back(new DestrCountRec(count), 5);
  v.emplace_back(new DestrCountRec(count), 5);
  deleter.AddObjects(v);

  deleter.FreeExpiredObjects(5);
  EXPECT_EQ(deleter.Count(), 2);
  EXPECT_EQ(count, 0);

  deleter.FreeExpiredObjects(6);
  EXPECT_EQ(deleter.Count(), 0);
  EXPECT_EQ(count, 2);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
