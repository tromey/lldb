//===-- RustFunctionCaller.h -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_RustFunctionCaller_h_
#define liblldb_RustFunctionCaller_h_

#include "lldb/Core/Address.h"
#include "lldb/Core/Value.h"
#include "lldb/Core/ValueObjectList.h"
#include "lldb/Expression/FunctionCaller.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Target/Process.h"
#include "Plugins/ExpressionParser/Clang/ClangFunctionCaller.h"

namespace lldb_private {

// Derive from ClangFunctionCaller so we don't have to reimplement
// ASTStructExtractor.  This is very naughty.
class RustFunctionCaller : public ClangFunctionCaller {

public:
  RustFunctionCaller(ExecutionContextScope &exe_scope,
                     const CompilerType &function_type,
                      const CompilerType &return_type,
                      const Address &function_address,
                      const ValueList &arg_value_list, const char *name);

  ~RustFunctionCaller() override;

  unsigned CompileFunction(lldb::ThreadSP thread_to_use_sp,
                           DiagnosticManager &diagnostic_manager) override;

private:

  CompilerType m_function_type;
};

} // namespace lldb_private

#endif // liblldb_RustFunctionCaller_h_
