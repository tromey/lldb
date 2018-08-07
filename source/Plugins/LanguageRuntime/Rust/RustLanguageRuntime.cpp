//===-- RustLanguageRuntime.cpp ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "RustLanguageRuntime.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/RustASTContext.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/Target.h"
#include "llvm/ADT/StringRef.h"

using namespace lldb;
using namespace lldb_private;

RustLanguageRuntime::RustLanguageRuntime(Process *process)
    : LanguageRuntime(process)
{
}

LanguageRuntime *
RustLanguageRuntime::CreateInstance(Process *process,
                                    lldb::LanguageType language) {
  if (language == eLanguageTypeRust)
    return new RustLanguageRuntime(process);
  return nullptr;
}

void RustLanguageRuntime::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(), "Rust language runtime",
                                CreateInstance);
}

void RustLanguageRuntime::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

lldb_private::ConstString RustLanguageRuntime::GetPluginNameStatic() {
  static ConstString g_name("rust");
  return g_name;
}

lldb_private::ConstString RustLanguageRuntime::GetPluginName() {
  return GetPluginNameStatic();
}

uint32_t RustLanguageRuntime::GetPluginVersion() {
  return 1;
}

bool RustLanguageRuntime::CouldHaveDynamicValue(ValueObject &in_value) {
  return in_value.GetCompilerType().IsPossibleDynamicType(nullptr, false, false);
}

bool RustLanguageRuntime::GetDynamicTypeAndAddress(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &dynamic_address,
    Value::ValueType &value_type) {
  class_type_or_name.Clear();
  value_type = Value::ValueType::eValueTypeScalar;

  CompilerType type = in_value.GetCompilerType();
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());

  if (!ast) {
    return false;
  }

  uint64_t discr_offset, discr_byte_size;
  if (ast->GetEnumDiscriminantLocation(type, discr_offset, discr_byte_size)) {
    lldb::addr_t original_ptr = in_value.GetAddressOf(false); // FIXME?
    if (original_ptr == LLDB_INVALID_ADDRESS) {
      return false;
    }

    ExecutionContext exe_ctx(in_value.GetExecutionContextRef());
    Process *process = exe_ctx.GetProcessPtr();
    if (process == nullptr) {
      return false;
    }

    Status error;
    uint64_t discriminant =
      process->ReadUnsignedIntegerFromMemory(original_ptr + discr_offset, discr_byte_size,
					     0, error);
    if (!error.Success()) {
      return false;
    }

    CompilerType variant_type = ast->FindEnumVariant(type, discriminant);
    if (!variant_type) {
      return false;
    }
    class_type_or_name = TypeAndOrName(variant_type);
    // The address doesn't change.
    dynamic_address.SetLoadAddress(original_ptr, exe_ctx.GetTargetPtr());
    value_type = Value::ValueType::eValueTypeLoadAddress;

    return true;
  }

  return false;
}

TypeAndOrName
RustLanguageRuntime::FixUpDynamicType(const TypeAndOrName &type_and_or_name,
                                      ValueObject &static_value) {
  return type_and_or_name;
}
