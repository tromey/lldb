//===-- RustUserExpression.h -------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_RustUserExpression_h
#define liblldb_RustUserExpression_h

#include <memory>
#include "lldb/Expression/UserExpression.h"
#include "Plugins/ExpressionParser/Rust/RustAST.h"

namespace lldb_private {

class RustUserExpression : public UserExpression {
public:

  RustUserExpression(ExecutionContextScope &exe_scope, llvm::StringRef expr,
                     llvm::StringRef prefix, lldb::LanguageType language,
                     ResultType desired_type,
                     const EvaluateExpressionOptions &options)
    : UserExpression(exe_scope, expr, prefix, language, desired_type, options)
  {
  }

  bool Parse(DiagnosticManager &diagnostic_manager,
             ExecutionContext &exe_ctx,
             lldb_private::ExecutionPolicy execution_policy,
             bool keep_result_in_memory, bool generate_debug_info) override;

  bool CanInterpret() override {
    return true;
  }

  // FIXME - what is this supposed to do.
  bool FinalizeJITExecution(DiagnosticManager &diagnostic_manager,
                            ExecutionContext &exe_ctx,
                            lldb::ExpressionVariableSP &result,
                            lldb::addr_t function_stack_bottom = LLDB_INVALID_ADDRESS,
                            lldb::addr_t function_stack_top = LLDB_INVALID_ADDRESS) override {
    return true;
  }

protected:

  lldb::ExpressionResults DoExecute(DiagnosticManager &diagnostic_manager,
                                    ExecutionContext &exe_ctx,
                                    const EvaluateExpressionOptions &options,
                                    lldb::UserExpressionSP &shared_ptr_to_me,
                                    lldb::ExpressionVariableSP &result) override;

private:

  RustExpressionUP m_expr;
};

} // namespace lldb_private

#endif // liblldb_RustUserExpression_h
