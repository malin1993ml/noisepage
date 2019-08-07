#include <gflags/gflags.h>
#include <tbb/task_scheduler_init.h>  // NOLINT
#include <unistd.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "execution/ast/ast_dump.h"
#include "execution/exec/execution_context.h"
#include "execution/exec/output.h"
#include "execution/parsing/parser.h"
#include "execution/parsing/scanner.h"
#include "execution/sema/error_reporter.h"
#include "execution/sema/sema.h"
#include "execution/sql/memory_pool.h"
#include "execution/sql/table_generator/sample_output.h"
#include "execution/sql/table_generator/table_generator.h"
#include "execution/tpl.h"  // NOLINT
#include "execution/util/cpu_info.h"
#include "execution/util/timer.h"
#include "execution/vm/bytecode_generator.h"
#include "execution/vm/bytecode_module.h"
#include "execution/vm/llvm_engine.h"
#include "execution/vm/module.h"
#include "execution/vm/vm.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "storage/garbage_collector.h"

#include "loggers/loggers_util.h"
#include "settings/settings_manager.h"

#define __SETTING_GFLAGS_DEFINE__      // NOLINT
#include "settings/settings_common.h"  // NOLINT
#include "settings/settings_defs.h"    // NOLINT
#undef __SETTING_GFLAGS_DEFINE__       // NOLINT

// ---------------------------------------------------------
// CLI options
// ---------------------------------------------------------

// clang-format off
llvm::cl::OptionCategory kTplOptionsCategory("TPL Compiler Options", "Options for controlling the TPL compilation process.");  // NOLINT
llvm::cl::opt<std::string> kInputFile(llvm::cl::Positional, llvm::cl::desc("<input file>"), llvm::cl::init(""), llvm::cl::cat(kTplOptionsCategory));  // NOLINT
llvm::cl::opt<bool> kPrintAst("print-ast", llvm::cl::desc("Print the programs AST"), llvm::cl::cat(kTplOptionsCategory));  // NOLINT
llvm::cl::opt<bool> kPrintTbc("print-tbc", llvm::cl::desc("Print the generated TPL Bytecode"), llvm::cl::cat(kTplOptionsCategory));  // NOLINT
llvm::cl::opt<std::string> kOutputName("output-name", llvm::cl::desc("Print the output name"), llvm::cl::init("schema1"), llvm::cl::cat(kTplOptionsCategory));  // NOLINT
llvm::cl::opt<bool> kIsSQL("sql", llvm::cl::desc("Is the input a SQL query?"), llvm::cl::cat(kTplOptionsCategory));  // NOLINT
// clang-format on

tbb::task_scheduler_init scheduler;

namespace tpl {

    class TplClass {
    public:
        static constexpr const char *kExitKeyword = ".exit";

        // Terrier objects
        terrier::transaction::TransactionManager * txn_manager_pointer_;
        exec::SampleOutput * sample_output_pointer_;
        terrier::catalog::db_oid_t db_oid_;
        terrier::catalog::Catalog * catalog_pointer_;

        TplClass(terrier::transaction::TransactionManager * txn_manager_pointer,
                 exec::SampleOutput * sample_output_pointer,
                 terrier::catalog::db_oid_t db_oid,
                 terrier::catalog::Catalog * catalog_pointer) :
                txn_manager_pointer_(txn_manager_pointer),
                sample_output_pointer_(sample_output_pointer),
                db_oid_(db_oid),
                catalog_pointer_(catalog_pointer) {}

/**
 * Compile the TPL source in \a source and run it in both interpreted and JIT
 * compiled mode
 * @param source The TPL source
 * @param name The name of the module/program
 */
        void CompileAndRun(const std::string &source, const std::string &name = "tmp-tpl") {

            auto *txn = txn_manager_pointer_->BeginTransaction();
            auto output_schema = sample_output_pointer_->GetSchema(kOutputName.data());
            exec::OutputPrinter printer(output_schema);
            auto accessor = std::unique_ptr<terrier::catalog::CatalogAccessor>(catalog_pointer_->GetAccessor(txn, db_oid_));
            exec::ExecutionContext exec_ctx{db_oid_, txn, printer, output_schema, std::move(accessor)};

            //-----------------------------------------
            // Let's scan the source
            util::Region region("repl-ast");
            util::Region error_region("repl-error");
            sema::ErrorReporter error_reporter(&error_region);
            ast::Context context(&region, &error_reporter);

            parsing::Scanner scanner(source.data(), source.length());
            parsing::Parser parser(&scanner, &context);

            double parse_ms = 0.0, typecheck_ms = 0.0, codegen_ms = 0.0, interp_exec_ms = 0.0, adaptive_exec_ms = 0.0,
                    jit_exec_ms = 0.0;

            //
            // Parse
            //

            ast::AstNode *root;
            {
                util::ScopedTimer <std::milli> timer(&parse_ms);
                root = parser.Parse();
            }

            if (error_reporter.HasErrors()) {
                EXECUTION_LOG_ERROR("Parsing error!");
                error_reporter.PrintErrors();
                return;
            }

            //
            // Type check
            //

            {
                util::ScopedTimer <std::milli> timer(&typecheck_ms);
                sema::Sema type_check(&context);
                type_check.Run(root);
            }

            if (error_reporter.HasErrors()) {
                EXECUTION_LOG_ERROR("Type-checking error!");
                error_reporter.PrintErrors();
                return;
            }

            // Dump AST
            if (kPrintAst) {
                ast::AstDump::Dump(root);
            }

            //
            // TBC generation
            //

            std::unique_ptr <vm::BytecodeModule> bytecode_module;
            {
                util::ScopedTimer <std::milli> timer(&codegen_ms);
                bytecode_module = vm::BytecodeGenerator::Compile(root, &exec_ctx, name);
            }

            // Dump Bytecode
            if (kPrintTbc) {
                bytecode_module->PrettyPrint(&std::cout);
            }

            auto module = std::make_unique<vm::Module>(std::move(bytecode_module));

            //
            // Interpret
            //

            {
                util::ScopedTimer <std::milli> timer(&interp_exec_ms);

                if (kIsSQL) {
                    std::function < i64(exec::ExecutionContext * ) > main;
                    if (!module->GetFunction("main", vm::ExecutionMode::Interpret, &main)) {
                        EXECUTION_LOG_ERROR(
                                "Missing 'main' entry function with signature "
                                "(*ExecutionContext)->int64");
                        return;
                    }
                    auto memory = std::make_unique<sql::MemoryPool>(nullptr);
                    exec_ctx.SetMemoryPool(std::move(memory));
                    EXECUTION_LOG_INFO("VM main() returned: {}", main(&exec_ctx));
                } else {
                    std::function < i64() > main;
                    if (!module->GetFunction("main", vm::ExecutionMode::Interpret, &main)) {
                        EXECUTION_LOG_ERROR("Missing 'main' entry function with signature ()->int64");
                        return;
                    }
                    EXECUTION_LOG_INFO("VM main() returned: {}", main());
                }
            }

            //
            // Adaptive
            //

            {
                util::ScopedTimer <std::milli> timer(&adaptive_exec_ms);

                if (kIsSQL) {
                    std::function < i64(exec::ExecutionContext * ) > main;
                    if (!module->GetFunction("main", vm::ExecutionMode::Adaptive, &main)) {
                        EXECUTION_LOG_ERROR(
                                "Missing 'main' entry function with signature "
                                "(*ExecutionContext)->int64");
                        return;
                    }
                    auto memory = std::make_unique<sql::MemoryPool>(nullptr);
                    exec_ctx.SetMemoryPool(std::move(memory));
                    EXECUTION_LOG_INFO("ADAPTIVE main() returned: {}", main(&exec_ctx));
                } else {
                    std::function < i64() > main;
                    if (!module->GetFunction("main", vm::ExecutionMode::Adaptive, &main)) {
                        EXECUTION_LOG_ERROR("Missing 'main' entry function with signature ()->int64");
                        return;
                    }
                    EXECUTION_LOG_INFO("ADAPTIVE main() returned: {}", main());
                }
            }

            //
            // JIT
            //
            {
                util::ScopedTimer <std::milli> timer(&jit_exec_ms);

                if (kIsSQL) {
                    std::function < i64(exec::ExecutionContext * ) > main;
                    if (!module->GetFunction("main", vm::ExecutionMode::Compiled, &main)) {
                        EXECUTION_LOG_ERROR(
                                "Missing 'main' entry function with signature "
                                "(*ExecutionContext)->int64");
                        return;
                    }
                    auto memory = std::make_unique<sql::MemoryPool>(nullptr);
                    exec_ctx.SetMemoryPool(std::move(memory));
                    EXECUTION_LOG_INFO("JIT main() returned: {}", main(&exec_ctx));
                } else {
                    std::function < i64() > main;
                    if (!module->GetFunction("main", vm::ExecutionMode::Compiled, &main)) {
                        EXECUTION_LOG_ERROR("Missing 'main' entry function with signature ()->int64");
                        return;
                    }
                    EXECUTION_LOG_INFO("JIT main() returned: {}", main());
                }
            }

            // Dump stats
            EXECUTION_LOG_INFO(
                    "Parse: {} ms, Type-check: {} ms, Code-gen: {} ms, Interp. Exec.: {} ms, "
                    "Adaptive Exec.: {} ms, Jit+Exec.: {} ms",
                    parse_ms, typecheck_ms, codegen_ms, interp_exec_ms, adaptive_exec_ms, jit_exec_ms);
            txn_manager_pointer_->Commit(txn, [](void *) {}, nullptr);
        }

    /**
     * Run the TPL REPL
     */
        void RunRepl() {
            while (true) {
                std::string input;

                std::string line;
                do {
                    printf(">>> ");
                    std::getline(std::cin, line);

                    if (line == kExitKeyword) {
                        return;
                    }

                    input.append(line).append("\n");
                } while (!line.empty());

                CompileAndRun(input);
            }
        }

    /**
     * Compile and run the TPL program in the given filename
     * @param filename The name of the file on disk to compile
     */
        void RunFile(const std::string &filename) {
            auto file = llvm::MemoryBuffer::getFile(filename);
            if (std::error_code error = file.getError()) {
                EXECUTION_LOG_ERROR("There was an error reading file '{}': {}", filename, error.message());
                return;
            }

            EXECUTION_LOG_INFO("Compiling and running file: {}", filename);

            // Copy the source into a temporary, compile, and run
            CompileAndRun((*file)->getBuffer().str());
        }


    /**
     * Shutdown all TPL subsystems
     */
        static void ShutdownTplClass() {
            tpl::vm::LLVMEngine::Shutdown();
            terrier::LoggersUtil::ShutDown();
            scheduler.terminate();
            LOG_INFO("TPL cleanly shutdown ...");
        }


        static void SignalHandler(i32 sig_num) {
            if (sig_num == SIGINT) {
                ShutdownTplClass();
                exit(0);
            }
        }

        static int InitTplClass(int argc, char **argv,
                                terrier::transaction::TransactionManager &txn_manager,
                                terrier::storage::BlockStore &block_store,
                                exec::SampleOutput &sample_output,
                                terrier::catalog::db_oid_t &db_oid,
                                terrier::catalog::Catalog &catalog) {  // NOLINT (bugprone-exception-escape)
            
            // Parse options
            llvm::cl::HideUnrelatedOptions(kTplOptionsCategory);
            llvm::cl::ParseCommandLineOptions(argc, argv); // here should be 1, {{"-sql"}}

            // Initialize a signal handler to call SignalHandler()
            struct sigaction sa;
            sa.sa_handler = &SignalHandler;
            sa.sa_flags = SA_RESTART;

            sigfillset(&sa.sa_mask);

            if (sigaction(SIGINT, &sa, nullptr) == -1) {
                EXECUTION_LOG_ERROR("Cannot handle SIGINT: {}", strerror(errno));
                return errno;
            }

            // Init TPL
            tpl::CpuInfo::Instance();

            //terrier::LoggersUtil::Initialize(false);

            tpl::vm::LLVMEngine::Initialize();

            EXECUTION_LOG_INFO("TPL Bytecode Count: {}", tpl::vm::Bytecodes::NumBytecodes());

            EXECUTION_LOG_INFO("TPL initialized ...");

            EXECUTION_LOG_INFO("\n{}", tpl::CpuInfo::Instance()->PrettyPrintInfo());

            EXECUTION_LOG_INFO("Welcome to TPL (ver. {}.{})", TPL_VERSION_MAJOR, TPL_VERSION_MINOR);

            // Either execute a TPL program from a source file, or run REPL
            /*
            if (!kInputFile.empty()) {
                tplclass::RunFile(kInputFile);
            } else if (argc == 1) {
                tpl::RunRepl();
            }

            return 0;*/


            auto txn = txn_manager.BeginTransaction();
            // Get the correct output format for this test
            sample_output.InitTestOutput();
            auto output_schema = sample_output.GetSchema(kOutputName.data());

            // Make the catalog accessor
            db_oid = catalog.CreateDatabase(txn, "test_db", true);
            auto accessor = std::unique_ptr<terrier::catalog::CatalogAccessor>(catalog.GetAccessor(txn, db_oid));
            auto ns_oid = accessor->CreateNamespace("test_ns");

            // Make the execution context
            exec::OutputPrinter printer(output_schema);
            exec::ExecutionContext exec_ctx{db_oid, txn, printer, output_schema, std::move(accessor)};

            // Generate test tables
            // TODO(Amadou): Read this in from a directory. That would require boost or experimental C++ though
            sql::TableGenerator table_generator{&exec_ctx, &block_store, ns_oid};
            table_generator.GenerateTestTables();
            table_generator.GenerateTableFromFile("../sample_tpl/tables/lineitem.schema",
                                                  "../sample_tpl/tables/lineitem.data");
            table_generator.GenerateTableFromFile("../sample_tpl/tables/types1.schema", "../sample_tpl/tables/types1.data");
            txn_manager.Commit(txn, [](void *) {}, nullptr);

            return 0;
        }

    };
}  // namespace tpl