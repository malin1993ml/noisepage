#include "execution/executable_query.h"

#include "execution/ast/ast_dump.h"
#include "execution/compiler/codegen.h"
#include "execution/compiler/compiler.h"
#include "execution/parsing/parser.h"
#include "execution/parsing/scanner.h"
#include "execution/sema/sema.h"
#include "execution/util/region.h"
#include "execution/vm/bytecode_generator.h"
#include "execution/vm/module.h"
#include "loggers/execution_logger.h"

namespace terrier::execution {

ExecutableQuery::ExecutableQuery(const common::ManagedPointer<planner::AbstractPlanNode> physical_plan,
                                 const common::ManagedPointer<exec::ExecutionContext> exec_ctx) {
  // Compile and check for errors
  compiler::CodeGen codegen(exec_ctx.Get());
  compiler::Compiler compiler(&codegen, physical_plan.Get());
  auto root = compiler.Compile();
  if (codegen.Reporter()->HasErrors()) {
    EXECUTION_LOG_ERROR("Type-checking error! \n {}", codegen.Reporter()->SerializeErrors());
    EXECUTION_LOG_ERROR("Dumping AST:");
    EXECUTION_LOG_ERROR(execution::ast::AstDump::Dump(root));
    return;
  }

  // Convert to bytecode
  auto bytecode_module = vm::BytecodeGenerator::Compile(root, exec_ctx.Get(), "tmp-tpl");

  tpl_module_ = std::make_unique<vm::Module>(std::move(bytecode_module));
  region_ = codegen.ReleaseRegion();
  ast_ctx_ = codegen.ReleaseContext();
}

ExecutableQuery::ExecutableQuery(const std::string &filename, const common::ManagedPointer<exec::ExecutionContext>
    exec_ctx) {
  auto file = llvm::MemoryBuffer::getFile(filename);
  if (std::error_code error = file.getError()) {
    EXECUTION_LOG_ERROR("There was an error reading file '{}': {}", filename, error.message());
    return;
  }

  // Copy the source into a temporary, compile, and run
  auto source = (*file)->getBuffer().str();

  // Let's scan the source
  region_ = std::make_unique<util::Region>("repl-ast");
  util::Region error_region("repl-error");
  sema::ErrorReporter error_reporter(&error_region);
  ast_ctx_ = std::make_unique<ast::Context>(region_.get(), &error_reporter);

  parsing::Scanner scanner(source.data(), source.length());
  parsing::Parser parser(&scanner, ast_ctx_.get());

  // Parse
  ast::AstNode *root = parser.Parse();
  if (error_reporter.HasErrors()) {
    EXECUTION_LOG_ERROR("Parsing errors: \n {}", error_reporter.SerializeErrors());
    throw std::runtime_error("Parsing Error!");
  }

  // Type check
  sema::Sema type_check(ast_ctx_.get());
  type_check.Run(root);
  if (error_reporter.HasErrors()) {
    EXECUTION_LOG_ERROR("Type-checking errors: \n {}", error_reporter.SerializeErrors());
    throw std::runtime_error("Type Checking Error!");
  }

  EXECUTION_LOG_DEBUG("Converted: \n {}", execution::ast::AstDump::Dump(root));

  // Convert to bytecode
  auto bytecode_module = vm::BytecodeGenerator::Compile(root, exec_ctx.Get(), "tmp-tpl");
  tpl_module_ = std::make_unique<vm::Module>(std::move(bytecode_module));

  // acquire the output format
  query_name_ = GetFileName(filename);
  sample_output_ = std::make_unique<exec::SampleOutput>();
  sample_output_->InitTestOutput();
  auto output_schema = sample_output_->GetSchema(query_name_);
  printer_ = std::make_unique<exec::OutputPrinter>(output_schema);
}

std::vector<type::TransientValue> ExecutableQuery::GetQueryParams() {
  std::vector<type::TransientValue> params;
  if (query_name_ == "tpch_q5")
    params.emplace_back(type::TransientValueFactory::GetVarChar("ASIA"));

  // Add the identifier for each pipeline. At most 8 query pipelines for now
  for (int i = 0; i < 8; ++i)
    params.emplace_back(type::TransientValueFactory::GetVarChar(query_name_ + "_p" + std::to_string(i + 1)));

  return params;
}

void ExecutableQuery::Run(const common::ManagedPointer<exec::ExecutionContext> exec_ctx, const vm::ExecutionMode mode) {
  TERRIER_ASSERT(tpl_module_ != nullptr, "Trying to run a module that failed to compile.");

  auto params = GetQueryParams();
  exec_ctx->SetParams(std::move(params));

  // Run the main function
  std::function<int64_t(exec::ExecutionContext *)> main;
  if (!tpl_module_->GetFunction("main", mode, &main)) {
    EXECUTION_LOG_ERROR(
        "Missing 'main' entry function with signature "
        "(*ExecutionContext)->int32");
    return;
  }
  auto result = main(exec_ctx.Get());
  EXECUTION_LOG_DEBUG("main() returned: {}", result);
}

}  // namespace terrier::execution