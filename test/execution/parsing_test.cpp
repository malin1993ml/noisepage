#include <utility>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "execution/tpl_test.h"  // NOLINT

#include "execution/ast/ast_dump.h"
#include "execution/parsing/parser.h"
#include "execution/parsing/scanner.h"

namespace tpl::parsing::test {

class ParserTest : public TplTest {
 public:
  ParserTest() : region_("test"), reporter_(&region_), ctx_(&region_, &reporter_) {}

  ast::Context *context() { return &ctx_; }
  sema::ErrorReporter *reporter() { return &reporter_; }

 private:
  util::Region region_;
  sema::ErrorReporter reporter_;
  ast::Context ctx_;
};

// NOLINTNEXTLINE
TEST_F(ParserTest, RegularForStmtTest) {
  const std::string source = R"(
    fun main() -> nil { for (var idx = 0; idx < 10; idx = idx + 1) { } }
  )";
  Scanner scanner(source);
  Parser parser(&scanner, context());

  // Attempt parse
  auto *ast = parser.Parse();
  ASSERT_NE(nullptr, ast);
  ASSERT_FALSE(reporter()->HasErrors());

  // No errors, move down AST
  ASSERT_TRUE(ast->IsFile());
  ASSERT_EQ(std::size_t{1}, ast->As<ast::File>()->declarations().size());

  // Only one function decl
  auto *decl = ast->As<ast::File>()->declarations()[0];
  ASSERT_TRUE(decl->IsFunctionDecl());

  auto *func_decl = decl->As<ast::FunctionDecl>();
  ASSERT_NE(nullptr, func_decl->function());
  ASSERT_NE(nullptr, func_decl->function()->body());
  ASSERT_EQ(std::size_t{1}, func_decl->function()->body()->statements().size());

  // Only one for statement, all elements are non-null
  auto *for_stmt = func_decl->function()->body()->statements()[0]->SafeAs<ast::ForStmt>();
  ASSERT_NE(nullptr, for_stmt);
  ASSERT_NE(nullptr, for_stmt->init());
  ASSERT_TRUE(for_stmt->init()->IsDeclStmt());
  ASSERT_TRUE(for_stmt->init()->As<ast::DeclStmt>()->declaration()->IsVariableDecl());
  ASSERT_NE(nullptr, for_stmt->condition());
  ASSERT_NE(nullptr, for_stmt->next());
}

// NOLINTNEXTLINE
TEST_F(ParserTest, ExhaustiveForStmtTest) {
  struct Test {
    const std::string source;
    bool init_null, cond_null, next_null;
    Test(std::string source, bool initNull, bool condNull, bool nextNull)
        : source(std::move(source)), init_null(initNull), cond_null(condNull), next_null(nextNull) {}
  };

  // All possible permutations of init, condition, and next statements in loops
  // clang-format off
  const Test tests[] = {
      {"fun main() -> nil { for (var idx = 0; idx < 10; idx = idx + 1) { } }", false, false, false},
      {"fun main() -> nil { for (var idx = 0; idx < 10; ) { } }",              false, false, true},
      {"fun main() -> nil { for (var idx = 0; ; idx = idx + 1) { } }",         false, true, false},
      {"fun main() -> nil { for (var idx = 0; ; ) { } }",                      false, true, true},
      {"fun main() -> nil { for (; idx < 10; idx = idx + 1) { } }",            true, false, false},
      {"fun main() -> nil { for (; idx < 10; ) { } }",                         true, false, true},
      {"fun main() -> nil { for (; ; idx = idx + 1) { } }",                    true, true, false},
      {"fun main() -> nil { for (; ; ) { } }",                                 true, true, true},
  };
  // clang-format on

  for (const auto &test : tests) {
    Scanner scanner(test.source);
    Parser parser(&scanner, context());

    // Attempt parse
    auto *ast = parser.Parse();
    ASSERT_NE(nullptr, ast);
    ASSERT_FALSE(reporter()->HasErrors());

    // No errors, move down AST
    ASSERT_TRUE(ast->IsFile());
    ASSERT_EQ(std::size_t{1}, ast->As<ast::File>()->declarations().size());

    // Only one function decl
    auto *decl = ast->As<ast::File>()->declarations()[0];
    ASSERT_TRUE(decl->IsFunctionDecl());

    auto *func_decl = decl->As<ast::FunctionDecl>();
    ASSERT_NE(nullptr, func_decl->function());
    ASSERT_NE(nullptr, func_decl->function()->body());
    ASSERT_EQ(std::size_t{1}, func_decl->function()->body()->statements().size());

    // Only one for statement, all elements are non-null
    auto *for_stmt = func_decl->function()->body()->statements()[0]->SafeAs<ast::ForStmt>();
    ASSERT_NE(nullptr, for_stmt);
    ASSERT_EQ(test.init_null, for_stmt->init() == nullptr);
    ASSERT_EQ(test.cond_null, for_stmt->condition() == nullptr);
    ASSERT_EQ(test.next_null, for_stmt->next() == nullptr);
  }
}

// NOLINTNEXTLINE
TEST_F(ParserTest, RegularForStmt_NoInitTest) {
  const std::string source = R"(
    fun main() -> nil {
      var idx = 0
      for (; idx < 10; idx = idx + 1) { }
    }
  )";
  Scanner scanner(source);
  Parser parser(&scanner, context());

  // Attempt parse
  auto *ast = parser.Parse();
  ASSERT_NE(nullptr, ast);
  ASSERT_FALSE(reporter()->HasErrors());

  // No errors, move down AST
  ASSERT_TRUE(ast->IsFile());
  ASSERT_EQ(std::size_t{1}, ast->As<ast::File>()->declarations().size());

  // Only one function decl
  auto *decl = ast->As<ast::File>()->declarations()[0];
  ASSERT_TRUE(decl->IsFunctionDecl());

  auto *func_decl = decl->As<ast::FunctionDecl>();
  ASSERT_NE(nullptr, func_decl->function());
  ASSERT_NE(nullptr, func_decl->function()->body());
  ASSERT_EQ(std::size_t{2}, func_decl->function()->body()->statements().size());

  // Two statements in function

  // First is the variable declaration
  auto &block = func_decl->function()->body()->statements();
  ASSERT_TRUE(block[0]->IsDeclStmt());
  ASSERT_TRUE(block[0]->As<ast::DeclStmt>()->declaration()->IsVariableDecl());

  // Next is the for statement
  auto *for_stmt = block[1]->SafeAs<ast::ForStmt>();
  ASSERT_NE(nullptr, for_stmt);
  ASSERT_EQ(nullptr, for_stmt->init());
  ASSERT_NE(nullptr, for_stmt->condition());
  ASSERT_NE(nullptr, for_stmt->next());
}

// NOLINTNEXTLINE
TEST_F(ParserTest, RegularForStmt_WhileTest) {
  const std::string for_while_sources[] = {
      R"(
      fun main() -> nil {
        var idx = 0
        for (idx < 10) { idx = idx + 1 }
      }
      )",
      R"(
      fun main() -> nil {
        var idx = 0
        for (; idx < 10; ) { idx = idx + 1 }
      }
      )",
  };

  for (const auto &source : for_while_sources) {
    Scanner scanner(source);
    Parser parser(&scanner, context());

    // Attempt parse
    auto *ast = parser.Parse();
    ASSERT_NE(nullptr, ast);
    ASSERT_FALSE(reporter()->HasErrors());

    // No errors, move down AST
    ASSERT_TRUE(ast->IsFile());
    ASSERT_EQ(std::size_t{1}, ast->As<ast::File>()->declarations().size());

    // Only one function decl
    auto *decl = ast->As<ast::File>()->declarations()[0];
    ASSERT_TRUE(decl->IsFunctionDecl());

    auto *func_decl = decl->As<ast::FunctionDecl>();
    ASSERT_NE(nullptr, func_decl->function());
    ASSERT_NE(nullptr, func_decl->function()->body());
    ASSERT_EQ(std::size_t{2}, func_decl->function()->body()->statements().size());

    // Two statements in function

    // First is the variable declaration
    auto &block = func_decl->function()->body()->statements();
    ASSERT_TRUE(block[0]->IsDeclStmt());
    ASSERT_TRUE(block[0]->As<ast::DeclStmt>()->declaration()->IsVariableDecl());

    // Next is the for statement
    auto *for_stmt = block[1]->SafeAs<ast::ForStmt>();
    ASSERT_NE(nullptr, for_stmt);
    ASSERT_EQ(nullptr, for_stmt->init());
    ASSERT_NE(nullptr, for_stmt->condition());
    ASSERT_EQ(nullptr, for_stmt->next());
  }
}

/*
TEST_F(ParserTest, RegularForInStmtTest) {
  const std::string source = R"(
    fun main() -> nil {
      for (idx in range()) { }
    }
  )";
  Scanner scanner(source);
  Parser parser(&scanner, context());

  // Attempt parse
  auto *ast = parser.Parse();
  ASSERT_NE(nullptr, ast);
  ASSERT_FALSE(reporter()->HasErrors());

  // No errors, move down AST
  ASSERT_TRUE(ast->IsFile());
  ASSERT_EQ(std::size_t{1}, ast->As<ast::File>()->declarations().size());

  // Only one function decl
  auto *decl = ast->As<ast::File>()->declarations()[0];
  ASSERT_TRUE(decl->IsFunctionDecl());

  auto *func_decl = decl->As<ast::FunctionDecl>();
  ASSERT_NE(nullptr, func_decl->function());
  ASSERT_NE(nullptr, func_decl->function()->body());
  ASSERT_EQ(std::size_t{1}, func_decl->function()->body()->statements().size());

  // Only statement is the for-in statement
  auto &block = func_decl->function()->body()->statements();
  auto *for_in_stmt = block[0]->SafeAs<ast::ForInStmt>();
  ASSERT_NE(nullptr, for_in_stmt);
  ASSERT_NE(nullptr, for_in_stmt->target());
  ASSERT_NE(nullptr, for_in_stmt->iter());
}
*/

// NOLINTNEXTLINE
TEST_F(ParserTest, ArrayTypeTest) {
  struct TestCase {
    std::string source;
    bool valid;
  };

  TestCase tests[] = {
      // Array with unknown length = valid
      {"fun main(arr: [*]int32) -> nil { }", true},
      // Array with known length = valid
      {"fun main() -> nil { var arr: [10]int32 }", true},
      // Array with missing length field = invalid
      {"fun main(arr: []int32) -> nil { }", false},
  };

  for (const auto &test_case : tests) {
    Scanner scanner(test_case.source);
    Parser parser(&scanner, context());

    // Attempt parse
    auto *ast = parser.Parse();
    ASSERT_NE(nullptr, ast);
    EXPECT_EQ(test_case.valid, !reporter()->HasErrors());
    reporter()->Reset();
  }
}

}  // namespace tpl::parsing::test
