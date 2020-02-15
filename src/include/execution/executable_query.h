#pragma once
#include <memory>
#include <utility>

#include "common/managed_pointer.h"
#include "execution/ast/context.h"
#include "execution/table_generator/sample_output.h"

namespace terrier::planner {
class AbstractPlanNode;
}

namespace terrier::execution {

namespace exec {
class ExecutionContext;
}

namespace vm {
enum class ExecutionMode : uint8_t;
class Module;
}  // namespace vm

namespace util {
class Region;
}

/**
 * ExecutableQuery abstracts the TPL code generation and compilation process. The result is an object that can be
 * invoked multiple times with multiple ExecutionContexts in multiple execution modes for as long its generated code is
 * valid (i.e. the objects to which it refers still exist).
 */
class ExecutableQuery {
 public:
  /**
   * Construct an executable query that maintains necessary state to be reused with multiple ExecutionContexts. It is up
   * to the owner to invalidate this object in the event that its references are no longer valid (schema change).
   * @param physical_plan output from the optimizer
   * @param exec_ctx execution context to use for code generation. Note that this execution context need not be the one
   * used for Run.
   */
  ExecutableQuery(common::ManagedPointer<planner::AbstractPlanNode> physical_plan,
                  common::ManagedPointer<exec::ExecutionContext> exec_ctx);

  /**
   * Construct and compile an executable TPL program in the given filename
   *
   * @param filename The name of the file on disk to compile
   * @param exec_ctx context to execute
   */
  ExecutableQuery(const std::string &filename, common::ManagedPointer<exec::ExecutionContext> exec_ctx);

  /**
   *
   * @param exec_ctx execution context to use for execution. Note that this execution context need not be the one used
   * for construction/codegen.
   * @param mode execution mode to use
   */
  void Run(common::ManagedPointer<exec::ExecutionContext> exec_ctx, vm::ExecutionMode mode);

  const planner::OutputSchema *GetOutputSchema() const { return sample_output_->GetSchema(query_name_);; }
  const exec::OutputPrinter &GetPrinter() const { return *printer_; }

  const std::string &GetQueryName() const {return query_name_; }

 private:
  static std::string GetFileName(const std::string &path) {
    std::size_t size = path.size();
    std::size_t found = path.find_last_of("/\\");
    return path.substr(found + 1, size - found - 5);
  }

  // TPL bytecodes for this query.
  std::unique_ptr<vm::Module> tpl_module_ = nullptr;

  // Memory region and AST context from the code generation stage that need to stay alive as long as the TPL module will
  // be executed. Direct access to these objects is likely unneeded from this class, we just want to tie the life cycles
  // together.
  std::unique_ptr<util::Region> region_;
  std::unique_ptr<ast::Context> ast_ctx_;

  // Used to specify the output for this query
  std::unique_ptr<exec::SampleOutput> sample_output_;
  std::unique_ptr<exec::OutputPrinter> printer_;
  std::string query_name_;
};
}  // namespace terrier::execution
