#include <memory>

#include "execution/sql_test.h"

#include "catalog/catalog_defs.h"
#include "execution/sql/index_iterator.h"
#include "execution/sql/table_vector_iterator.h"
#include "execution/util/timer.h"

namespace terrier::execution::sql::test {

class IndexIteratorTest : public SqlBasedTest {
  void SetUp() override {
    // Create the test tables
    SqlBasedTest::SetUp();
    exec_ctx_ = MakeExecCtx();
    GenerateTestTables(exec_ctx_.get());
  }

 protected:
  /**
   * Execution context to use for the test
   */
  std::unique_ptr<exec::ExecutionContext> exec_ctx_;
};

// NOLINTNEXTLINE
TEST_F(IndexIteratorTest, SimpleIndexIteratorTest) {
  //
  // Access table data through the index
  //

  auto table_oid = exec_ctx_->GetAccessor()->GetTableOid(NSOid(), "test_1");
  const auto &schema = exec_ctx_->GetAccessor()->GetSchema(table_oid);
  auto index_oid = exec_ctx_->GetAccessor()->GetIndexOid(NSOid(), "index_1");
  TableVectorIterator table_iter(!table_oid, exec_ctx_.get());
  IndexIterator index_iter{!table_oid, !index_oid, exec_ctx_.get()};
  table_iter.AddCol(!schema.GetColumn("colA").Oid());
  index_iter.AddCol(!schema.GetColumn("colA").Oid());
  table_iter.Init();
  index_iter.Init();
  ProjectedColumnsIterator *pci = table_iter.projected_columns_iterator();

  // Iterate through the table.
  while (table_iter.Advance()) {
    for (; pci->HasNext(); pci->Advance()) {
      auto *key = pci->Get<i32, false>(0, nullptr);
      // Check that the key can be recovered through the index
      index_iter.SetKey<i32>(0, *key, false);
      index_iter.ScanKey();
      // One entry should be found
      ASSERT_TRUE(index_iter.Advance());
      auto *val = index_iter.Get<i32, false>(0, nullptr);
      ASSERT_EQ(*key, *val);
      // Check that there are no more entries.
      ASSERT_FALSE(index_iter.Advance());
    }
    pci->Reset();
  }
}

}  // namespace terrier::execution::sql::test
