//===-- RustParse.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "RustParse.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/RustASTContext.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/Status.h"
#include <functional>
#include <set>

#include "Plugins/ExpressionParser/Clang/ASTStructExtractor.h"
#include "RustFunctionCaller.h"

using namespace lldb_private::rust;
using namespace lldb_private;
using namespace lldb;
using namespace llvm;

static std::set<std::string> primitive_type_names
  {
   "bool", "char",
   "u8", "u16", "u32", "u64", "u128",
   "i8", "i16", "i32", "i64", "i128",
   "f32", "f64"
  };

static RustASTContext *
GetASTContext(CompilerType type, Status &error) {
  RustASTContext *result = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());
  if (!result) {
    error.SetErrorString("not a Rust type!?");
  }
  return result;
}

static RustASTContext *
GetASTContext(ValueObjectSP val, Status &error) {
  return GetASTContext(val->GetCompilerType(), error);
}

static RustASTContext *
GetASTContext(ExecutionContext &ctxt, Status &error) {
  Target *target = ctxt.GetTargetPtr();
  TypeSystem *sys = target->GetScratchTypeSystemForLanguage(&error, eLanguageTypeRust);
  if (!sys) {
    return nullptr;
  }
  RustASTContext *result = llvm::dyn_cast_or_null<RustASTContext>(sys);
  if (!result) {
    error.SetErrorString("not a Rust type!?");
  }
  return result;
}

static ValueObjectSP
CreateValueFromScalar(ExecutionContext &exe_ctx, Scalar &scalar, CompilerType type,
		      Status &error) {
  DataExtractor data;
  if (!scalar.GetData(data)) {
    error.SetErrorString("could not get data from scalar");
    return ValueObjectSP();
  }
  ValueObjectSP result = ValueObject::CreateValueObjectFromData("", data, exe_ctx, type);
  if (!result) {
    error.SetErrorString("could not create value object");
  }
  return result;
}

static ValueObjectSP
CreateValueInMemory(ExecutionContext &exe_ctx, CompilerType type, Status &error) {
  if (!exe_ctx.HasProcessScope()) {
    error.SetErrorString("need a running inferior to evaluate this");
    return ValueObjectSP();
  }

  Process *proc = exe_ctx.GetProcessPtr();
  uint64_t size = type.GetByteSize(proc);
  addr_t addr = proc->AllocateMemory(size,
                                     lldb::ePermissionsWritable | lldb::ePermissionsReadable,
                                     error);
  if (addr == LLDB_INVALID_ADDRESS) {
    return ValueObjectSP();
  }

  return ValueObject::CreateValueObjectFromAddress("", addr, exe_ctx, type);
}

static bool
SetField(const ValueObjectSP &object, const char *name, uint64_t value, Status &error) {
  Scalar scalar(value);
  DataExtractor data;
  if (!scalar.GetData(data)) {
    error.SetErrorString("could not get data from scalar");
    return false;
  }

  ValueObjectSP child = object->GetChildMemberWithName(ConstString(name), true);
  if (!child) {
    error.SetErrorStringWithFormat("could not find child named \"%s\"", name);
    return false;
  }
  return child->SetData(data, error);
}

static bool
SetField(const ValueObjectSP &object, const char *name, const ValueObjectSP &value,
         Status &error) {
  DataExtractor data;
  if (!value->GetData(data, error)) {
    return false;
  }

  ValueObjectSP child = object->GetChildMemberWithName(ConstString(name), true);
  if (!child) {
    error.SetErrorStringWithFormat("could not find child named \"%s\"", name);
    return false;
  }
  return child->SetData(data, error);
}

static CompilerType
GetTypeByName(ExecutionContext &exe_ctx, const char *name, Status &error) {
  Target *target = exe_ctx.GetTargetPtr();
  if (!target) {
    error.SetErrorString("could not get target to look up type");
    return CompilerType();
  }

  SymbolContext sc;
  TypeList type_list;
  llvm::DenseSet<SymbolFile *> searched_symbol_files;
  uint32_t num_matches = target->GetImages().FindTypes(sc, ConstString(name), true,
                                                       2, searched_symbol_files, type_list);
  if (num_matches > 0) {
    return type_list.GetTypeAtIndex(0)->GetFullCompilerType();
  }
  error.SetErrorStringWithFormat("could not find type \"%s\"", name);
  return CompilerType();
}

ValueObjectSP
lldb_private::rust::UnaryDereference(ExecutionContext &exe_ctx, ValueObjectSP addr, Status &error) {
  return addr->Dereference(error);
}

ValueObjectSP
lldb_private::rust::UnaryAddr(ExecutionContext &exe_ctx, ValueObjectSP val, Status &error) {
  return val->AddressOf(error);
}

ValueObjectSP
lldb_private::rust::UnaryPlus(ExecutionContext &exe_ctx, ValueObjectSP val, Status &error) {
  if (RustASTContext *ast = GetASTContext(val, error)) {
    CompilerType type = val->GetCompilerType();
    if (type.IsScalarType() && !ast->IsBooleanType(type.GetOpaqueQualType())) {
      return val;
    }
    error.SetErrorString("not a scalar type");
  }
  return ValueObjectSP();
}

ValueObjectSP
lldb_private::rust::UnaryNegate(ExecutionContext &exe_ctx, ValueObjectSP val, Status &error) {
  if (RustASTContext *ast = GetASTContext(val, error)) {
    CompilerType type = val->GetCompilerType();
    if (!type.IsScalarType() || ast->IsBooleanType(type.GetOpaqueQualType())) {
      error.SetErrorString("not a scalar type");
      return ValueObjectSP();
    }

    Scalar scalar;
    if (!val->ResolveValue(scalar)) {
      error.SetErrorString("could not resolve scalar value");
      return ValueObjectSP();
    }
    if (!scalar.UnaryNegate()) {
      error.SetErrorString("could not negate scalar value");
      return ValueObjectSP();
    }

    return CreateValueFromScalar(exe_ctx, scalar, type, error);
  }
  return ValueObjectSP();
}

ValueObjectSP
lldb_private::rust::UnaryComplement(ExecutionContext &exe_ctx, ValueObjectSP val, Status &error) {
  RustASTContext *ast = GetASTContext(val, error);
  if (!ast) {
    return ValueObjectSP();
  }

  CompilerType type = val->GetCompilerType();
  if (!type.IsScalarType()) {
    error.SetErrorString("not a scalar type");
    return ValueObjectSP();
  }

  Scalar scalar;
  if (!val->ResolveValue(scalar)) {
    error.SetErrorString("could not resolve scalar value");
    return ValueObjectSP();
  }

  if (ast->IsBooleanType(type.GetOpaqueQualType())) {
    scalar = Scalar(int(scalar.IsZero()));
  } else if (!scalar.OnesComplement()) {
    error.SetErrorString("could not negate scalar value");
    return ValueObjectSP();
  }

  return CreateValueFromScalar(exe_ctx, scalar, type, error);
}

ValueObjectSP
lldb_private::rust::UnarySizeof(ExecutionContext &exe_ctx, ValueObjectSP val, Status &error) {
  if (RustASTContext *ast = GetASTContext(val, error)) {
    uint32_t ptr_size = ast->GetPointerByteSize();
    CompilerType type = ast->CreateIntegralType(ConstString("usize"), false, ptr_size);
    Scalar size (val->GetByteSize());
    return CreateValueFromScalar(exe_ctx, size, type, error);
  }
  return ValueObjectSP();
}

template<typename T, bool ASSIGN>
ValueObjectSP
lldb_private::rust::BinaryOperation (ExecutionContext &exe_ctx, lldb::ValueObjectSP left,
                                     lldb::ValueObjectSP right, Status &error) {
  RustASTContext *ast = GetASTContext(left, error);
  if (!ast) {
    return ValueObjectSP();
  }

  if (!left->GetCompilerType().IsScalarType() || !right->GetCompilerType().IsScalarType()) {
    error.SetErrorString("not a scalar type");
    return ValueObjectSP();
  }

  Scalar sleft, sright;
  if (!left->ResolveValue(sleft) || !right->ResolveValue(sright)) {
    error.SetErrorString("could not resolve scalar value");
    return ValueObjectSP();
  }

  Scalar result = T()(sleft, sright);
  if (result.GetType() == Scalar::e_void) {
    error.SetErrorString("could not resolve scalar value");
    return ValueObjectSP();
  }

  size_t byte_size = result.GetByteSize();
  CompilerType type;

  // FIXME there has to be a better way.
  switch (result.GetType()) {
  case Scalar::e_sint:
  case Scalar::e_slong:
  case Scalar::e_slonglong:
    type = ast->CreateIntrinsicIntegralType(true, byte_size);
    break;

  case Scalar::e_uint:
  case Scalar::e_ulong:
  case Scalar::e_ulonglong:
    type = ast->CreateIntrinsicIntegralType(false, byte_size);
    break;

  case Scalar::e_float:
  case Scalar::e_double:
    if (byte_size == 4) {
      type = ast->CreateFloatType(ConstString("f32"), byte_size);
      break;
    } else if (byte_size == 8) {
      type = ast->CreateFloatType(ConstString("f64"), byte_size);
      break;
    }
    /* FALL THROUGH */

  default:
    error.SetErrorString("unknown type resulting from binary operation");
    return ValueObjectSP();
  }

  ValueObjectSP result_obj = CreateValueFromScalar(exe_ctx, result, type, error);

  if (ASSIGN) {
    DataExtractor data;
    result_obj->GetData(data, error);
    if (error.Fail()) {
      return ValueObjectSP();
    }

    if (!left->SetData(data, error)) {
      return ValueObjectSP();
    }

    result_obj = left;
  }

  return result_obj;
}

template<typename T>
ValueObjectSP
lldb_private::rust::Comparison (ExecutionContext &exe_ctx, lldb::ValueObjectSP left,
                                lldb::ValueObjectSP right, Status &error) {
  RustASTContext *ast = GetASTContext(left, error);
  if (!ast) {
    return ValueObjectSP();
  }

  if (!left->GetCompilerType().IsScalarType() || !right->GetCompilerType().IsScalarType()) {
    error.SetErrorString("not a scalar type");
    return ValueObjectSP();
  }

  Scalar sleft, sright;
  if (!left->ResolveValue(sleft) || !right->ResolveValue(sright)) {
    error.SetErrorString("could not resolve scalar value");
    return ValueObjectSP();
  }

  bool result = T()(sleft, sright);
  Scalar value = int(result);

  CompilerType type = ast->CreateBoolType(ConstString("bool"));
  return CreateValueFromScalar(exe_ctx, value, type, error);
}

ValueObjectSP
lldb_private::rust::ArrayIndex (ExecutionContext &exe_ctx, lldb::ValueObjectSP left,
                                lldb::ValueObjectSP right, Status &error) {
  if (!right->GetCompilerType().IsScalarType()) {
    error.SetErrorString("not a scalar type");
    return ValueObjectSP();
  }
  if (RustASTContext *ast = GetASTContext(right, error)) {
    CompilerType type = right->GetCompilerType();
    if (ast->IsBooleanType(type.GetOpaqueQualType())) {
      error.SetErrorString("not a scalar type");
      return ValueObjectSP();
    }
  }

  Scalar sright;
  if (!right->ResolveValue(sright)) {
    error.SetErrorString("could not resolve scalar value");
    return ValueObjectSP();
  }
  unsigned long index = sright.ULong(-1);

  ValueObjectSP result = left->GetChildAtIndex(index, true);
  if (!result) {
    error.SetErrorString("array index out of bounds");
  }
  return result;
}

CompilerDeclContext
RustPath::FrameDeclContext(ExecutionContext &exe_ctx, Status &error) {
  StackFrame *frame = exe_ctx.GetFramePtr();
  if (frame == nullptr) {
    // FIXME?
    error.SetErrorString("no frame when looking up item");
    return CompilerDeclContext();
  }

  SymbolContext sym_ctx = frame->GetSymbolContext(lldb::eSymbolContextFunction |
                                                  lldb::eSymbolContextBlock);

  if (sym_ctx.block) {
    CompilerDeclContext frame_decl_context = sym_ctx.block->GetDeclContext();
    if (frame_decl_context) {
      return frame_decl_context;
    }
  }

  error.SetErrorString("could not find frame's decl context");
  return CompilerDeclContext();
}

bool
RustPath::GetDeclContext(ExecutionContext &exe_ctx, Status &error,
                         CompilerDeclContext *result, bool *simple_name) {
  *simple_name = true;

  RustASTContext *ast = GetASTContext(exe_ctx, error);
  if (!ast) {
    return false;
  }

  CompilerDeclContext decl_ctx = FrameDeclContext(exe_ctx, error);
  if (!decl_ctx) {
    return false;
  }

  if (!m_relative || m_self || m_supers > 0) {
    for (int i = 0; !m_relative || i < m_supers; ++i) {
      CompilerDeclContext next = ast->GetDeclContextDeclContext(decl_ctx);
      if (next.GetName().IsEmpty()) {
        if (!m_relative) {
          break;
        }
        error.SetErrorString("too many 'super's");
        return false;
      }
      decl_ctx = next;
    }

    *simple_name = false;
    *result = decl_ctx;
  }

  *result = decl_ctx;
  return true;
}

bool
RustPath::AppendGenerics(ExecutionContext &exe_ctx, Status &error, std::string *name) {
  if (!m_generic_params.empty()) {
    *name += "<";
    bool first = true;
    for (const RustTypeExpressionUP &param : m_generic_params) {
      CompilerType type = param->Evaluate(exe_ctx, error);
      if (!type) {
        return false;
      }
      if (!first) {
        *name += ", ";
      }
      first = false;
      *name += type.GetTypeName().AsCString();
    }
    *name += ">";
  }
  return true;
}

bool
RustPath::FindDecl(ExecutionContext &exe_ctx, Status &error,
                   lldb::VariableSP *var,
                   lldb_private::Function **function,
                   std::string *base_name) {
  bool simple_name;
  CompilerDeclContext decl_ctx;
  if (!GetDeclContext(exe_ctx, error, &decl_ctx, &simple_name)) {
    return false;
  }

  if (m_path.size() > 1) {
    simple_name = false;
  }

  std::string name = m_path.back();
  if (!AppendGenerics(exe_ctx, error, &name)) {
    return false;
  }

  if (simple_name) {
    *base_name = name;
    // ... but still look in the current context.
  }

  RustASTContext *ast = GetASTContext(exe_ctx, error);
  if (!ast) {
    return false;
  }

  // Construct the fully-qualified name.
  std::vector<ConstString> fullname;
  while (decl_ctx.GetName()) {
    fullname.push_back(decl_ctx.GetName());
    decl_ctx = ast->GetDeclContextDeclContext(decl_ctx);
  }
  std::reverse(fullname.begin(), fullname.end());

  auto end_iter = m_path.end() - 1;
  for (auto iter = m_path.begin(); iter != end_iter; ++iter) {
    fullname.push_back(ConstString(iter->c_str()));
  }

  // Now try to find this name in each Module.
  Target *target = exe_ctx.GetTargetPtr();
  if (!target) {
    error.SetErrorString("could not get target to look up item");
    return false;
  }

  VariableList var_list;
  *function = nullptr;
  ConstString cs_name(name.c_str());
  const ModuleList &module_list = target->GetImages();
  module_list.ForEach(
    [&](const ModuleSP &mod) {
      TypeSystem *ts = mod->GetTypeSystemForLanguage(eLanguageTypeRust);
      if (!ts) {
        return true;
      }
      SymbolFile *symbol_file = ts->GetSymbolFile();
      if (!symbol_file) {
        return true;
      }

      SymbolContext null_sc;
      CompilerDeclContext found_ns;
      for (const ConstString &ns_name : fullname) {
        found_ns = symbol_file->FindNamespace(null_sc, ns_name, &found_ns);
        if (!found_ns) {
          break;
        }
      }

      if (found_ns) {
        mod->FindGlobalVariables(cs_name, &found_ns, 1, var_list);

        SymbolContextList context_list;
        mod->FindFunctions(cs_name, &found_ns, eFunctionNameTypeBase, false, false,
                           true, context_list);
        for (size_t i = 0; i < context_list.GetSize(); ++i) {
          SymbolContext sym_context;
          if (context_list.GetContextAtIndex(i, sym_context) && sym_context.function) {
            *function = sym_context.function;
            break;
          }
        }
      }

      return var_list.GetSize() == 0 && *function == nullptr;
    });

  if (var_list.GetSize() != 0) {
    *var = var_list.GetVariableAtIndex(0);
  } else if (*function != nullptr) {
    // Ok.
  } else {
    if (base_name->empty()) {
      error.SetErrorStringWithFormat("could not find decl \"%s\"", name.c_str());
    }
    return false;
  }

  return true;
}

CompilerType
RustPath::EvaluateAsType(ExecutionContext &exe_ctx, Status &error) {
  std::string fullname = Name(exe_ctx, error);
  if (error.Fail()) {
    return CompilerType();
  }
  return GetTypeByName(exe_ctx, fullname.c_str(), error);
}

std::string
RustPath::Name(ExecutionContext &exe_ctx, Status &error) {
  std::string name;

  CompilerDeclContext decl_ctx;
  bool ignore;
  if (!GetDeclContext(exe_ctx, error, &decl_ctx, &ignore)) {
    return std::string();
  }

  if (m_path.size() == 1 &&
      primitive_type_names.find(m_path[0]) != primitive_type_names.end()) {
    // Primitive.
    return m_path[0];
  }

  // FIXME local types
  name += "::";
  name += decl_ctx.GetScopeQualifiedName().AsCString();
  name += "::";

  {
    bool first = true;
    for (const std::string &str : m_path) {
      if (!first) {
        name += "::";
      }
      first = false;
      name += str;
    }
  }

  if (!AppendGenerics(exe_ctx, error, &name)) {
    return std::string();
  }

  return name;
}

lldb::ValueObjectSP
RustLiteral::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  CompilerType type = m_type->Evaluate(exe_ctx, error);
  if (!type) {
    return ValueObjectSP();
  }
  return CreateValueFromScalar(exe_ctx, m_value, type, error);
}

lldb::ValueObjectSP
RustBooleanLiteral::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  RustASTContext *ast = GetASTContext(exe_ctx, error);
  if (!ast) {
    return ValueObjectSP();
  }

  CompilerType type = ast->CreateBoolType(ConstString("bool"));
  Scalar val(m_value);
  return CreateValueFromScalar(exe_ctx, val, type, error);
}

lldb::ValueObjectSP
RustCharLiteral::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  RustASTContext *ast = GetASTContext(exe_ctx, error);
  if (!ast) {
    return ValueObjectSP();
  }

  CompilerType type;
  if (m_is_byte) {
    type = ast->CreateIntegralType(ConstString("u8"), false, 1);
  } else {
    type = ast->CreateCharType();
  }
  Scalar val(m_value);
  return CreateValueFromScalar(exe_ctx, val, type, error);
}

lldb::ValueObjectSP
RustStringLiteral::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  RustASTContext *ast = GetASTContext(exe_ctx, error);
  if (!ast) {
    return ValueObjectSP();
  }

  CompilerType u8 = ast->CreateIntegralType(ConstString("u8"), false, 1);
  CompilerType array_type = ast->CreateArrayType(u8, m_value.size());
  if (!array_type) {
    error.SetErrorString("could not create array type");
    return ValueObjectSP();
  }

  // Byte order and address size don't matter here.
  DataExtractor data(m_value.c_str(), m_value.size(), eByteOrderInvalid, 4);
  ValueObjectSP array = CreateValueInMemory(exe_ctx, array_type, error);
  if (!array) {
    return array;
  }

  if (!array->SetData(data, error)) {
    return ValueObjectSP();
  }

  if (m_is_byte) {
    return array;
  }

  CompilerType str_type = GetTypeByName(exe_ctx, "&str", error);
  if (!str_type) {
    return ValueObjectSP();
  }

  ValueObjectSP str_val = CreateValueInMemory(exe_ctx, str_type, error);
  if (!str_val) {
    return str_val;
  }

  if (!SetField(str_val, "data_ptr", array->GetAddressOf(), error) ||
      !SetField(str_val, "length", m_value.size(), error)) {
    return ValueObjectSP();
  }

  return str_val;
}

lldb::ValueObjectSP
RustPathExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  Target *target = exe_ctx.GetTargetPtr();
  if (!target) {
    error.SetErrorString("could not get target to look up item");
    return ValueObjectSP();
  }

  StackFrame *frame = exe_ctx.GetFramePtr();
  if (frame == nullptr) {
    // FIXME?
    error.SetErrorString("no frame when looking up item");
    return ValueObjectSP();
  }

  std::string name;
  VariableSP decl;
  Function *function;
  m_path->FindDecl(exe_ctx, error, &decl, &function, &name);
  if (error.Fail()) {
    return ValueObjectSP();
  }

  if (!name.empty()) {
    VariableListSP frame_vars = frame->GetInScopeVariableList(false);
    if (frame_vars) {
      if (VariableSP var = frame_vars->FindVariable(ConstString(name.c_str()))) {
        // FIXME dynamic?  should come from the options, which we aren't
        // passing in.
        return frame->GetValueObjectForFrameVariable(var, eDynamicDontRunTarget);
      }
    }
  }
  if (decl) {
    return frame->TrackGlobalVariable(decl, eDynamicDontRunTarget);
  }

  if (function) {
    Address addr = function->GetAddressRange().GetBaseAddress();
    addr_t address = addr.GetCallableLoadAddress(target);
    CompilerType type = function->GetCompilerType().GetPointerType();
    Scalar saddr(address);
    return CreateValueFromScalar(exe_ctx, saddr, type, error);
  }

  // FIXME use the name
  error.SetErrorStringWithFormat("could not find item");
  return ValueObjectSP();
}

lldb::ValueObjectSP
RustAndAndExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  ValueObjectSP vleft = m_left->Evaluate(exe_ctx, error);
  if (!vleft) {
    return vleft;
  }

  if (RustASTContext *ast = GetASTContext(vleft, error)) {
    CompilerType type = vleft->GetCompilerType();
    if (!ast->IsBooleanType(type.GetOpaqueQualType())) {
      error.SetErrorString("not a boolean type");
      return ValueObjectSP();
    }

    Scalar sleft;
    if (!vleft->ResolveValue(sleft)) {
      error.SetErrorString("could not resolve scalar value");
      return ValueObjectSP();
    }
    if (sleft.IsZero()) {
      return vleft;
    }
  } else {
    return ValueObjectSP();
  }

  ValueObjectSP vright = m_right->Evaluate(exe_ctx, error);
  if (!vright) {
    return vright;
  }

  // FIXME should probably error out if not boolean.
  return vright;
}

lldb::ValueObjectSP
RustOrOrExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  ValueObjectSP vleft = m_left->Evaluate(exe_ctx, error);
  if (!vleft) {
    return vleft;
  }

  if (RustASTContext *ast = GetASTContext(vleft, error)) {
    CompilerType type = vleft->GetCompilerType();
    if (!ast->IsBooleanType(type.GetOpaqueQualType())) {
      error.SetErrorString("not a boolean type");
      return ValueObjectSP();
    }

    Scalar sleft;
    if (!vleft->ResolveValue(sleft)) {
      error.SetErrorString("could not resolve scalar value");
      return ValueObjectSP();
    }
    if (!sleft.IsZero()) {
      return vleft;
    }
  } else {
    return ValueObjectSP();
  }

  ValueObjectSP vright = m_right->Evaluate(exe_ctx, error);
  if (!vright) {
    return vright;
  }

  // FIXME should probably error out if not boolean.
  return vright;
}

lldb::ValueObjectSP
RustFieldExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  ValueObjectSP left = m_left->Evaluate(exe_ctx, error);
  if (!left) {
    return left;
  }

  // We always want to let users see the real type, because in Rust
  // only trait objects and enums can be dynamic.
  ValueObjectSP dynamic = left->GetDynamicValue(eDynamicCanRunTarget);
  if (dynamic) {
    left = dynamic;
  }

  ValueObjectSP result = left->GetChildMemberWithName(ConstString(m_field.c_str()), true);
  if (!result) {
    error.SetErrorStringWithFormat("no field named %s", m_field.c_str());
  }
  return result;
}

lldb::ValueObjectSP
RustTupleFieldExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  ValueObjectSP left = m_left->Evaluate(exe_ctx, error);
  if (!left) {
    return left;
  }

  // We always want to let users see the real type, because in Rust
  // only trait objects and enums can be dynamic.
  ValueObjectSP dynamic = left->GetDynamicValue(eDynamicCanRunTarget);
  if (dynamic) {
    left = dynamic;
  }

  ValueObjectSP result = left->GetChildAtIndex(m_field, true);
  if (!result) {
    error.SetErrorStringWithFormat("no field number %d", m_field);
  }
  return result;
}

lldb::ValueObjectSP
RustAssignment::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  ValueObjectSP left = m_left->Evaluate(exe_ctx, error);
  if (!left) {
    return left;
  }

  ValueObjectSP right = m_right->Evaluate(exe_ctx, error);
  if (!right) {
    return right;
  }

  DataExtractor data;
  right->GetData(data, error);
  if (error.Fail()) {
    return ValueObjectSP();
  }

  if (!left->SetData(data, error)) {
    return ValueObjectSP();
  }

  return left;
}

lldb::ValueObjectSP
RustTupleExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  error.SetErrorString("tuple expressions unimplemented");
  return ValueObjectSP();
}

lldb::ValueObjectSP
RustArrayLiteral::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  error.SetErrorString("array literals unimplemented");
  return ValueObjectSP();
}

lldb::ValueObjectSP
RustArrayWithLength::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  ValueObjectSP value = m_value->Evaluate(exe_ctx, error);
  if (!value) {
    return value;
  }
  ValueObjectSP length = m_length->Evaluate(exe_ctx, error);
  if (!length) {
    return length;
  }
  Scalar slength;
  if (!length->ResolveValue(slength)) {
    error.SetErrorString("could not resolve scalar value");
    return ValueObjectSP();
  }

  RustASTContext *ast = GetASTContext(value, error);
  if (!ast) {
    return ValueObjectSP();
  }
  CompilerType type = ast->CreateArrayType(value->GetCompilerType(), slength.UInt());
  if (!type) {
    error.SetErrorString("could not create array type");
    return ValueObjectSP();
  }

  DataExtractor data;
  value->GetData(data, error);
  if (error.Fail()) {
    return ValueObjectSP();
  }

  DataExtractor array_contents;
  for (unsigned i = 0; i < slength.UInt(); ++i) {
    if (!array_contents.Append(data)) {
      error.SetErrorString("could not create array contents");
      return ValueObjectSP();
    }
  }

  ValueObjectSP result = ValueObject::CreateValueObjectFromData("", array_contents, exe_ctx, type);
  if (!result) {
    error.SetErrorString("could not create array value object");
  }
  return result;
}

lldb::ValueObjectSP
RustCall::MaybeEvalTupleStruct(ExecutionContext &exe_ctx, Status &error) {
  RustPathExpression *path_expr = m_func->AsPath();
  if (!path_expr) {
    return ValueObjectSP();
  }

  CompilerType type = path_expr->EvaluateAsType(exe_ctx, error);
  if (!type) {
    // If we didn't find this as a type, keep trying as a function
    // call.
    error.Clear();
    return ValueObjectSP();
  }

  // After this point, all errors are real.

  RustASTContext *context = GetASTContext(type, error);
  if (!context) {
    return ValueObjectSP();
  }
  if (!context->IsTupleType(type)) {
    error.SetErrorString("not a tuple type");
    return ValueObjectSP();
  }

  if (m_exprs.size() < type.GetNumFields()) {
    error.SetErrorString("not enough initializers for tuple");
    return ValueObjectSP();
  } else if (m_exprs.size() > type.GetNumFields()) {
    error.SetErrorString("too many initializers for tuple");
    return ValueObjectSP();
  }

  ValueObjectSP result = CreateValueInMemory(exe_ctx, type, error);
  if (!result) {
    return result;
  }

  for (size_t i = 0; i < m_exprs.size(); ++i) {
    ValueObjectSP init_val = m_exprs[i]->Evaluate(exe_ctx, error);
    if (!init_val) {
      return init_val;
    }

    DataExtractor data;
    if (!init_val->GetData(data, error)) {
      error.SetErrorString("could not get data from value");
      return ValueObjectSP();
    }

    ValueObjectSP child = result->GetChildAtIndex(i, true);
    if (!child) {
      error.SetErrorStringWithFormat("could not find child at index \"%d\"", int(i));
      return ValueObjectSP();
    }
    if (!child->SetData(data, error)) {
      return ValueObjectSP();
    }
  }

  return result;
}

lldb::ValueObjectSP
RustCall::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  error.Clear();
  ValueObjectSP result = MaybeEvalTupleStruct(exe_ctx, error);
  if (result) {
    return result;
  } else if (error.Fail()) {
    // This looked like a tuple struct expression but failed.
    return result;
  }
  // This was not a tuple struct expression, so fall through to
  // function call.

  ValueObjectSP func = m_func->Evaluate(exe_ctx, error);
  if (!func) {
    return func;
  }
  if (!func->GetCompilerType().IsFunctionPointerType()) {
    error.SetErrorString("not calling a function");
    return ValueObjectSP();
  }
  CompilerType function_type = func->GetCompilerType().GetPointeeType();
  CompilerType return_type = function_type.GetFunctionReturnType();

  if (m_exprs.size() < function_type.GetNumberOfFunctionArguments()) {
    error.SetErrorString("too few arguments to function");
    return ValueObjectSP();
  } else if (m_exprs.size() > function_type.GetNumberOfFunctionArguments()) {
    error.SetErrorString("too many arguments to function");
    return ValueObjectSP();
  }

  addr_t addr = func->GetPointerValue();
  Address func_addr(addr);

  std::vector<ValueObjectSP> hold;
  ValueList args;
  for (auto &&arg : m_exprs) {
    ValueObjectSP varg = arg->Evaluate(exe_ctx, error);
    if (!varg) {
      return varg;
    }
    hold.push_back(varg);

    // FIXME - mega hack to work around FunctionCaller limitation.
    // Ideally this would be hidden in RustFunctionCaller at least.
    if (varg->GetCompilerType().IsAggregateType()) {
      varg = varg->AddressOf(error);
      if (!varg) {
        return varg;
      }
      hold.push_back(varg);
    }

    args.PushValue(varg->GetValue());
  }

  // FIXME must cast each argument to the correct type here.

  // FIXME might be nice to stick the name in there.
  RustFunctionCaller call(*exe_ctx.GetBestExecutionContextScope(), function_type,
                          return_type, func_addr, args, nullptr);
  DiagnosticManager diags;
  Value results;
  ExpressionResults ef_result =
    call.ExecuteFunction(exe_ctx, nullptr, EvaluateExpressionOptions(), diags, results);

  if (ef_result != eExpressionCompleted) {
    // FIXME use the diagnostics.
    error.SetErrorString("function call failed");
    return ValueObjectSP();
  }

  DataExtractor data;
  if (!results.GetData(data)) {
    error.SetErrorString("could not extract return value");
    return ValueObjectSP();
  }

  result = ValueObject::CreateValueObjectFromData("", data, exe_ctx, return_type);
  if (!result) {
    error.SetErrorString("could not create function return value object");
  }
  return result;
}

lldb::ValueObjectSP
RustCast::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  CompilerType type = m_type->Evaluate(exe_ctx, error);
  if (!type) {
    return ValueObjectSP();
  }

  ValueObjectSP value = m_expr->Evaluate(exe_ctx, error);
  if (!value) {
    return value;
  }

  return value->Cast(type);
}

lldb::ValueObjectSP
RustStructExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  CompilerType type = m_path->Evaluate(exe_ctx, error);
  if (!type) {
    return ValueObjectSP();
  }
  RustASTContext *context = GetASTContext(type, error);
  if (!context) {
    return ValueObjectSP();
  }
  // FIXME could tighten this to really ensure it is a struct and not
  // an enum.
  if (!type.IsAggregateType() ||
      type.IsArrayType(nullptr, nullptr, nullptr) ||
      context->IsTupleType(type)) {
    error.SetErrorStringWithFormat("type \"%s\" is not a structure type",
                                   type.GetDisplayTypeName().AsCString());
    return ValueObjectSP();
  }

  ValueObjectSP result = CreateValueInMemory(exe_ctx, type, error);
  if (!result) {
    return result;
  }

  if (m_copy) {
    ValueObjectSP copy = m_copy->Evaluate(exe_ctx, error);
    if (!copy) {
      return copy;
    }

    DataExtractor data;
    copy->GetData(data, error);
    if (error.Fail()) {
      return ValueObjectSP();
    }

    if (!result->SetData(data, error)) {
      return ValueObjectSP();
    }
  } else {
    if (m_inits.size() != type.GetNumFields()) {
      error.SetErrorStringWithFormat("some initializers missing for \"%s\"",
                                     type.GetDisplayTypeName().AsCString());
      return ValueObjectSP();
    }
  }

  for (const auto &init : m_inits) {
    ValueObjectSP init_val = init.second->Evaluate(exe_ctx, error);
    if (!init_val) {
      return init_val;
    }
    if (!SetField(result, init.first.c_str(), init_val, error)) {
      return ValueObjectSP();
    }
  }

  return result;
}

lldb::ValueObjectSP
RustRangeExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  ValueObjectSP left, right;

  if (m_left) {
    left = m_left->Evaluate(exe_ctx, error);
    if (!left) {
      return left;
    }
  }
  if (m_right) {
    right = m_right->Evaluate(exe_ctx, error);
    if (!right) {
      return right;
    }
  }

  std::string name;
  CompilerType element_type;
  if (left) {
    if (right) {
      name = m_inclusive ? "::core::ops::range::RangeInclusive" : "::core::ops::range::Range";
      // FIXME this check seems wrong
      if (left->GetCompilerType() != right->GetCompilerType()) {
        // FIXME also we could be friendlier about integer promotion
        // here.
        error.SetErrorString("operands of \"..\" do not have same type");
        return ValueObjectSP();
      }
    } else {
      name = "::core::ops::range::RangeFrom";
    }
    element_type = left->GetCompilerType();
  } else if (right) {
    name = m_inclusive ? "::core::ops::range::RangeToInclusive" : "::core::ops::range::RangeTo";
    element_type = right->GetCompilerType();
  } else {
    name = "::core::ops::range::RangeFull";
  }
  assert(!name.empty());

  if (element_type) {
    name = name + "<" + element_type.GetTypeName().AsCString() + ">";
  }

  CompilerType range_type = GetTypeByName(exe_ctx, name.c_str(), error);
  if (!range_type) {
    return ValueObjectSP();
  }

  ValueObjectSP result = CreateValueInMemory(exe_ctx, range_type, error);
  if (!result) {
    return result;
  }

  if (left && !SetField(result, "start", left, error)) {
    return ValueObjectSP();
  }
  if (right && !SetField(result, "end", right, error)) {
    return ValueObjectSP();
  }

  return result;
}

////////////////////////////////////////////////////////////////
// Types

CompilerType
RustArrayTypeExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  RustASTContext *context = GetASTContext(exe_ctx, error);
  if (!context) {
    return CompilerType();
  }

  CompilerType element = m_element->Evaluate(exe_ctx, error);
  if (!element) {
    return element;
  }

  return context->CreateArrayType(element, m_len);
}

CompilerType
RustPointerTypeExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  CompilerType target = m_target->Evaluate(exe_ctx, error);
  if (!target) {
    return target;
  }
  // FIXME references
  return target.GetPointerType();
}

CompilerType
RustSliceTypeExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  error.SetErrorString("slice type lookup unimplemented");
  return CompilerType();
}

CompilerType
RustFunctionTypeExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  RustASTContext *context = GetASTContext(exe_ctx, error);
  if (!context) {
    return CompilerType();
  }

  CompilerType ret = m_result->Evaluate(exe_ctx, error);
  if (!ret) {
    return ret;
  }

  std::vector<CompilerType> args;
  for (const RustTypeExpressionUP &arg : m_arguments) {
    CompilerType argtype = arg->Evaluate(exe_ctx, error);
    if (!argtype) {
      return argtype;
    }
    args.push_back(argtype);
  }

  return context->CreateFunctionType(ConstString(""), ret, std::move(args));
}

CompilerType
RustTupleTypeExpression::Evaluate(ExecutionContext &exe_ctx, Status &error) {
  error.SetErrorString("tuple type lookup unimplemented");
  return CompilerType();
}

////////////////////////////////////////////////////////////////
// Output

Stream &lldb_private::operator<< (Stream &stream, const RustExpressionUP &expr) {
  if (expr) {
    expr->print(stream);
  }
  return stream;
}

Stream &lldb_private::operator<< (Stream &stream, const RustTypeExpressionUP &type) {
  type->print(stream);
  return stream;
}

Stream &lldb_private::operator<< (Stream &stream, const Scalar &value) {
  value.GetValue(&stream, false);
  return stream;
}

Stream &lldb_private::operator<< (Stream &stream,
                                  const std::pair<std::string, RustExpressionUP> &value) {
  return stream << value.first << ": " << value.second;
}

////////////////////////////////////////////////////////////////
// The parser

template<char C, RustUnaryOperator OP>
RustExpressionUP Parser::Unary(Status &error) {
  Advance();
  RustExpressionUP result = Term(error);
  if (!result) {
    return result;
  }
  return llvm::make_unique<RustUnaryExpression<C, OP>>(std::move(result));
}

bool Parser::ExprList(std::vector<RustExpressionUP> *exprs, Status &error) {
  while (true) {
    RustExpressionUP expr = Expr(error);
    if (!expr) {
      return false;
    }

    exprs->push_back(std::move(expr));
    if (CurrentToken().kind != ',') {
      break;
    }
    Advance();
  }

  return true;
}

// This handles both a parenthesized expression and a tuple
// expression.
RustExpressionUP Parser::Parens(Status &error) {
  assert(CurrentToken().kind == '(');
  Advance();

  if (CurrentToken().kind == ')') {
    // Unit tuple.
    Advance();
    return llvm::make_unique<RustTupleExpression>(std::vector<RustExpressionUP>());
  }

  RustExpressionUP expr = Expr(error);
  if (!expr) {
    return expr;
  }

  if (CurrentToken().kind == ')') {
    // Parenthesized expression.
    Advance();
    return expr;
  } else if (CurrentToken().kind != ',') {
    error.SetErrorString("expected ')' or ','");
    return RustExpressionUP();
  }

  std::vector<RustExpressionUP> exprs;
  exprs.push_back(std::move(expr));
  Advance();

  if (CurrentToken().kind != ')') {
    if (!ExprList(&exprs, error)) {
      return RustExpressionUP();
    }
  }

  if (CurrentToken().kind != ')') {
    error.SetErrorString("expected ')'");
    return RustExpressionUP();
  }
  Advance();

  return llvm::make_unique<RustTupleExpression>(std::move(exprs));
}

RustExpressionUP Parser::Array(Status &error) {
  assert(CurrentToken().kind == '[');
  Advance();

  // This doesn't affect us, so parse and ignore.
  if (CurrentToken().kind == MUT) {
    Advance();
  }

  RustExpressionUP expr = Expr(error);
  if (!expr) {
    return expr;
  }

  RustExpressionUP result;
  if (CurrentToken().kind == ';') {
    Advance();

    RustExpressionUP length = Expr(error);
    if (!length) {
      return length;
    }

    result = llvm::make_unique<RustArrayWithLength>(std::move(expr), std::move(length));
  } else if (CurrentToken().kind == ',') {
    Advance();
    std::vector<RustExpressionUP> exprs;
    exprs.push_back(std::move(expr));

    if (ExprList(&exprs, error)) {
      result = llvm::make_unique<RustArrayLiteral>(std::move(exprs));
    }
  } else {
    error.SetErrorString("expected ',' or ';'");
    return result;
  }

  if (CurrentToken().kind != ']') {
    error.SetErrorString("expected ']'");
    result.reset();
  } else {
    Advance();
  }

  return result;
}

RustExpressionUP Parser::Field(RustExpressionUP &&lhs, Status &error) {
  assert(CurrentToken().kind == '.');
  Advance();

  RustExpressionUP result;
  if (CurrentToken().kind == IDENTIFIER) {
    result = llvm::make_unique<RustFieldExpression>(std::move(lhs),
                                                    CurrentToken().str);
    Advance();
  } else if (CurrentToken().kind == INTEGER) {
    result = llvm::make_unique<RustTupleFieldExpression>(std::move(lhs),
                                                         CurrentToken().uinteger.getValue());
    Advance();
  } else {
    error.SetErrorString("identifier or integer expected");
  }

  return result;
}

RustExpressionUP Parser::Call(RustExpressionUP &&func, Status &error) {
  assert(CurrentToken().kind == '(');
  Advance();

  std::vector<RustExpressionUP> exprs;
  if (CurrentToken().kind != ')') {
    if (!ExprList(&exprs, error)) {
      return RustExpressionUP();
    }
  }

  if (CurrentToken().kind != ')') {
    error.SetErrorString("expected ')'");
    return RustExpressionUP();
  }
  Advance();

  return llvm::make_unique<RustCall>(std::move(func), std::move(exprs));
}

RustExpressionUP Parser::Index(RustExpressionUP &&array, Status &error) {
  assert(CurrentToken().kind == '[');
  Advance();

  RustExpressionUP idx = Expr(error);
  if (!idx) {
    return idx;
  }

  if (CurrentToken().kind != ']') {
    error.SetErrorString("expected ']'");
    return RustExpressionUP();
  }
  Advance();

  return llvm::make_unique<RustBinaryExpression<'@', ArrayIndex>>(std::move(array),
                                                                  std::move(idx));
}

RustExpressionUP Parser::Struct(RustTypeExpressionUP &&path, Status &error) {
  assert(CurrentToken().kind == '{');
  Advance();

  std::vector<std::pair<std::string, RustExpressionUP>> inits;
  RustExpressionUP copy;
  while (CurrentToken().kind != '}') {
    if (CurrentToken().kind == IDENTIFIER) {
      std::string field = std::move(CurrentToken().str);
      Advance();

      RustExpressionUP value;
      if (CurrentToken().kind == ',' || CurrentToken().kind == '}') {
        // Plain "field".
        std::string field_copy = field;
        value = llvm::make_unique<RustPathExpression>(std::move(field_copy));
      } else if (CurrentToken().kind != ':') {
        error.SetErrorString("':' expected");
        return RustExpressionUP();
      } else {
        Advance();

        value = Expr(error);
        if (!value) {
          return value;
        }
      }

      // FIXME look for duplicates.

      inits.emplace_back(std::move(field), std::move(value));
    } else if (CurrentToken().kind == DOTDOT) {
      // FIXME technically this can't occur first - a field
      // initializer is needed.
      Advance();
      copy = Expr(error);
      if (!copy) {
        return copy;
      }
      break;
    } else {
      error.SetErrorString("identifier or '..' expected");
      return RustExpressionUP();
    }

    if (CurrentToken().kind != ',') {
      break;
    }
    Advance();
  }

  if (CurrentToken().kind != '}') {
    error.SetErrorString("'}' expected");
    return RustExpressionUP();
  }
  Advance();

  return llvm::make_unique<RustStructExpression>(std::move(path), std::move(inits),
                                                 std::move(copy));
}

RustExpressionUP Parser::Path(Status &error) {
  bool relative = true;
  int supers = 0;

  bool saw_self = CurrentToken().kind == SELF;
  if (saw_self) {
    Advance();
    if (CurrentToken().kind != COLONCOLON) {
      // This one can't be a struct expression, so we just return
      // directly.
      return llvm::make_unique<RustPathExpression>(std::string("self"));
    }
  }

  if (CurrentToken().kind == COLONCOLON) {
    if (!saw_self) {
      relative = false;
    }
    Advance();
  }

  if (relative) {
    while (CurrentToken().kind == SUPER) {
      ++supers;
      Advance();
      if (CurrentToken().kind != COLONCOLON) {
        error.SetErrorString("'::' expected after 'super'");
        return RustExpressionUP();
      }
      Advance();
    }
  }

  std::vector<std::string> path;
  while (CurrentToken().kind == IDENTIFIER) {
    path.emplace_back(std::move(CurrentToken().str));
    Advance();
    if (CurrentToken().kind != COLONCOLON) {
      break;
    }
    Advance();
    if (CurrentToken().kind != IDENTIFIER && CurrentToken().kind != '<') {
      error.SetErrorString("identifier or '<' expected");
      return RustExpressionUP();
    }
  }

  std::vector<RustTypeExpressionUP> type_list;
  if (CurrentToken().kind == '<') {
    if (!BracketTypeList(&type_list, error)) {
      return RustExpressionUP();
    }
  }

  if (path.empty()) {
    error.SetErrorString("identifier expected");
    return RustExpressionUP();
  }

  if (CurrentToken().kind == '{') {
    RustPathUP name_path = llvm::make_unique<RustPath>(saw_self, relative, supers,
                                                       std::move(path), std::move(type_list),
                                                       true);
    RustTypeExpressionUP type_path =
      llvm::make_unique<RustPathTypeExpression>(std::move(name_path));
    return Struct(std::move(type_path), error);
  }

  RustPathUP name_path = llvm::make_unique<RustPath>(saw_self, relative, supers,
                                                     std::move(path), std::move(type_list));
  return llvm::make_unique<RustPathExpression>(std::move(name_path));
}

RustExpressionUP Parser::Sizeof(Status &error) {
  assert(CurrentToken().kind == SIZEOF);
  Advance();

  if (CurrentToken().kind != '(') {
    error.SetErrorString("'(' expected");
    return RustExpressionUP();
  }
  Advance();

  RustExpressionUP expr = Expr(error);
  if (!expr) {
    return expr;
  }

  if (CurrentToken().kind != ')') {
    error.SetErrorString("')' expected");
    return RustExpressionUP();
  }
  Advance();

  return llvm::make_unique<RustUnaryExpression<'@', UnarySizeof>>(std::move(expr));
}

bool Parser::StartsTerm() {
  // Must be kept in parallel with the switch in Term.
  switch (CurrentToken().kind) {
  case INTEGER:
  case FLOAT:
  case STRING:
  case BYTESTRING:
  case CHAR:
  case BYTE:
  case TRUE:
  case FALSE:
  case '[':
  case '(':
  case SUPER:
  case SELF:
  case IDENTIFIER:
  case COLONCOLON:
  case SIZEOF:
  case '*':
  case '+':
  case '-':
  case '!':
  case '&':
    return true;

  default:
    return false;
  }
}

RustExpressionUP Parser::Term(Status &error) {
  RustExpressionUP term;

  // Double-check StartsTerm.
  bool starts = StartsTerm();

  switch (CurrentToken().kind) {
  case INTEGER: {
    const char *suffix = CurrentToken().number_suffix;
    if (!suffix) {
      suffix = "i32";
    }
    RustTypeExpressionUP type = llvm::make_unique<RustPathTypeExpression>(suffix);
    term = llvm::make_unique<RustLiteral>(CurrentToken().uinteger.getValue(), std::move(type));
    Advance();
    break;
  }

  case FLOAT: {
    const char *suffix = CurrentToken().number_suffix;
    if (!suffix) {
      suffix = "f64";
    }
    RustTypeExpressionUP type = llvm::make_unique<RustPathTypeExpression>(suffix);
    term = llvm::make_unique<RustLiteral>(CurrentToken().dvalue.getValue(), std::move(type));
    Advance();
    break;
  }

  case STRING:
  case BYTESTRING:
    term = llvm::make_unique<RustStringLiteral>(std::move(CurrentToken().str),
                                                CurrentToken().kind == BYTESTRING);
    Advance();
    break;

  case CHAR:
  case BYTE:
    term = llvm::make_unique<RustCharLiteral>(CurrentToken().uinteger.getValue(),
                                              CurrentToken().kind == BYTE);
    Advance();
    break;

  case TRUE:
  case FALSE:
    term = llvm::make_unique<RustBooleanLiteral>(CurrentToken().kind == TRUE);
    Advance();
    break;

  case '[':
    term = Array(error);
    break;

  case '(':
    term = Parens(error);
    break;

  case SUPER:
  case SELF:
  case IDENTIFIER:
  case COLONCOLON:
    term = Path(error);
    break;

  case SIZEOF:
    assert(starts);
    return Sizeof(error);

  case '*':
    term = Unary<'*', UnaryDereference>(error);
    break;
  case '+':
    term = Unary<'+', UnaryPlus>(error);
    break;
  case '-':
    term = Unary<'-', UnaryNegate>(error);
    break;
  case '!':
    term = Unary<'!', UnaryComplement>(error);
    break;

  case '&':
    // FIXME should handle slices here.
    term = Unary<'&', UnaryAddr>(error);
    break;

  case INVALID:
    assert(!starts);
    error.SetErrorString(CurrentToken().str);
    return RustExpressionUP();

  case THATSALLFOLKS:
    assert(!starts);
    error.SetErrorString("unexpected EOF");
    return RustExpressionUP();

  default:
    assert(!starts);
    error.SetErrorString("unexpected token");
    return RustExpressionUP();
  }

  assert(starts);

  bool done = false;
  while (!done) {
    switch (CurrentToken().kind) {
    case AS: {
      Advance();
      RustTypeExpressionUP type = Type(error);
      if (!type) {
        return RustExpressionUP();
      }
      term = llvm::make_unique<RustCast>(std::move(type), std::move(term));
      break;
    }

    case '.':
      term = Field(std::move(term), error);
      break;

    case '[':
      term = Index(std::move(term), error);
      break;

    case '(':
      term = Call(std::move(term), error);
      break;

    default:
      done = true;
      break;
    }

    if (!term) {
      return term;
    }
  }

  return term;
}

#define BINOP(Tag, What)                                        \
  RustBinaryExpression< Tag, BinaryOperation< What<Scalar>, false > >
#define COMP(Tag, What)                                         \
  RustBinaryExpression< Tag, Comparison< What<Scalar> > >
#define ASSIGN(Tag, What)                                       \
  RustAssignExpression< Tag, BinaryOperation< What<Scalar>, true > >

// Couldn't find these in <functional>.

template<typename T>
class left_shift {
public:
  T operator()(const T &l, const T&r) {
    return l << r;
  }
};

template<typename T>
class right_shift {
public:
  T operator()(const T &l, const T&r) {
    return l >> r;
  }
};


// Binary operators.  Each line has the form:
//   DEFINE(token, precedence, type)
#define BINARY_OPERATORS                                        \
  DEFINE(OROR, 3, RustOrOrExpression)                           \
  DEFINE(ANDAND, 4, RustAndAndExpression)                       \
  DEFINE(EQEQ, 5, COMP(EQEQ, std::equal_to))                    \
  DEFINE(NOTEQ, 5, COMP(NOTEQ, std::not_equal_to))              \
  DEFINE(LTEQ, 5, COMP(LTEQ, std::less_equal))                  \
  DEFINE(GTEQ, 5, COMP(GTEQ, std::greater_equal))               \
  DEFINE(LSH, 9, BINOP(LSH, left_shift))                        \
  DEFINE(RSH, 9, BINOP(RSH, right_shift))                       \
  DEFINE(PLUS_EQ, 1, ASSIGN(PLUS_EQ, std::plus))                \
  DEFINE(MINUS_EQ, 1, ASSIGN(MINUS_EQ, std::minus))             \
  DEFINE(SLASH_EQ, 1, ASSIGN(SLASH_EQ, std::divides))           \
  DEFINE(STAR_EQ, 1, ASSIGN(STAR_EQ, std::multiplies))          \
  DEFINE(PERCENT_EQ, 1, ASSIGN(PERCENT_EQ, std::modulus))       \
  DEFINE(RSH_EQ, 1, ASSIGN(RSH_EQ, right_shift))                \
  DEFINE(LSH_EQ, 1, ASSIGN(LSH_EQ, left_shift))                 \
  DEFINE(AND_EQ, 1, ASSIGN(AND_EQ, std::bit_and))               \
  DEFINE(OR_EQ, 1, ASSIGN(OR_EQ, std::bit_or))                  \
  DEFINE(XOR_EQ, 1, ASSIGN(XOR_EQ, std::bit_xor))               \
  DEFINE('|', 6, BINOP('|', std::bit_or))                       \
  DEFINE('&', 8, BINOP('&', std::bit_and))                      \
  DEFINE('=', 1, RustAssignment)                                \
  DEFINE('<', 5, COMP('<', std::less))                          \
  DEFINE('>', 5, COMP('>', std::greater))                       \
  DEFINE('+', 10, BINOP('+', std::plus))                        \
  DEFINE('-', 10, BINOP('-', std::minus))                       \
  DEFINE('*', 11, BINOP('*', std::multiplies))                  \
  DEFINE('/', 11, BINOP('/', std::divides))                     \
  DEFINE('%', 11, BINOP('%', std::modulus))

RustExpressionUP Parser::Binary(Status &error) {
  struct Operation {
    int precedence;
    int op;
    RustExpressionUP term;

    Operation(int precedence_, int op_, RustExpressionUP &&term_)
      : precedence(precedence_),
        op(op_),
        term(std::move(term_))
    {
    }
  };

  RustExpressionUP term = Term(error);
  if (!term) {
    return term;
  }

  std::vector<Operation> operations;
  operations.emplace_back(0, -1, std::move(term));

  bool done = false;
  while (!done) {
    int precedence;
    int kind = CurrentToken().kind;

    switch (kind) {
#define DEFINE(Token, Prec, Type)                       \
      case Token: precedence = Prec; Advance(); break;

      BINARY_OPERATORS
#undef DEFINE

    case INVALID:
      error.SetErrorString(CurrentToken().str);
      return RustExpressionUP();

    case THATSALLFOLKS:
    default:
      // Not a binary operator, so we're going to return.
      done = true;
      // Arrange to pop all the operations now.
      precedence = 0;
      break;
    }

    RustExpressionUP rhs;
    if (!done) {
      rhs = Term(error);
      if (!rhs) {
        return rhs;
      }
    }

    while (precedence < operations.back().precedence) {
      Operation top = std::move(operations.back());
      operations.pop_back();

      assert(!operations.empty());
      Operation &lhs = operations.back();

      switch (top.op) {
#define DEFINE(Token, Prec, Type)                                       \
        case Token:                                                     \
          lhs.term = llvm::make_unique<Type>(std::move(lhs.term), std::move(top.term)); \
          break;

        BINARY_OPERATORS
#undef DEFINE

      default:
        assert(0);
      }
    }

    // This won't be true in the "done" case.
    if (rhs) {
      operations.emplace_back(precedence, kind, std::move(rhs));
    }
  }

  assert(operations.size() == 1);
  return std::move(operations.back().term);
}

RustExpressionUP Parser::Range(Status &error) {
  RustExpressionUP lhs;
  if (CurrentToken().kind != DOTDOT && CurrentToken().kind != DOTDOTEQ) {
    lhs = Binary(error);
    if (!lhs) {
      return lhs;
    }
  }

  if (CurrentToken().kind != DOTDOT && CurrentToken().kind != DOTDOTEQ) {
    return lhs;
  }
  bool is_inclusive = CurrentToken().kind == DOTDOTEQ;
  Advance();

  // An inclusive range requires an expression, but an exclusive range
  // does not.
  RustExpressionUP rhs;
  if (is_inclusive || StartsTerm()) {
    rhs = Binary(error);
    if (!rhs) {
      return rhs;
    }
  }

  return llvm::make_unique<RustRangeExpression>(std::move(lhs), std::move(rhs), is_inclusive);
}

////////////////////////////////////////////////////////////////
// Type parsing

RustTypeExpressionUP Parser::ArrayType(Status &error) {
  assert(CurrentToken().kind == '[');
  Advance();

  RustTypeExpressionUP element = Type(error);
  if (!element) {
    return element;
  }

  if (CurrentToken().kind != ';') {
    error.SetErrorString("';' expected");
    return RustTypeExpressionUP();
  }
  Advance();

  if (CurrentToken().kind != INTEGER) {
    error.SetErrorString("integer expected");
    return RustTypeExpressionUP();
  }

  uint64_t len = CurrentToken().uinteger.getValue();
  Advance();

  if (CurrentToken().kind != ']') {
    error.SetErrorString("']' expected");
    return RustTypeExpressionUP();
  }
  Advance();

  return llvm::make_unique<RustArrayTypeExpression>(std::move(element), len);
}

RustTypeExpressionUP Parser::ReferenceType(Status &error) {
  assert(CurrentToken().kind == '&');
  Advance();

  bool is_mut = false;
  if (CurrentToken().kind == MUT) {
    is_mut = true;
    Advance();
  }

  bool is_slice = false;
  if (CurrentToken().kind == '[') {
    is_slice = true;
    Advance();
  }

  RustTypeExpressionUP target = Type(error);
  if (!target) {
    return target;
  }

  if (is_slice) {
    if (CurrentToken().kind != ']') {
      error.SetErrorString("']' expected");
      return RustTypeExpressionUP();
    }
    Advance();

    return llvm::make_unique<RustSliceTypeExpression>(std::move(target), is_mut);
  }

  return llvm::make_unique<RustPointerTypeExpression>(std::move(target), true, is_mut);
}

RustTypeExpressionUP Parser::PointerType(Status &error) {
  assert(CurrentToken().kind == '*');
  Advance();

  bool is_mut = false;
  if (CurrentToken().kind == MUT) {
    is_mut = true;
  } else if (CurrentToken().kind != CONST) {
    error.SetErrorString("expected 'mut' or 'const'");
    return RustTypeExpressionUP();
  }
  Advance();

  RustTypeExpressionUP target = Type(error);
  if (!target) {
    return target;
  }

  return llvm::make_unique<RustPointerTypeExpression>(std::move(target), false, is_mut);
}

bool Parser::TypeList(std::vector<RustTypeExpressionUP> *type_list, Status &error) {
  while (true) {
    RustTypeExpressionUP t = Type(error);
    if (!t) {
      return false;
    }
    type_list->push_back(std::move(t));
    if (CurrentToken().kind != ',') {
      break;
    }
    Advance();
  }

  return true;
}

bool Parser::ParenTypeList(std::vector<RustTypeExpressionUP> *type_list, Status &error) {
  if (CurrentToken().kind != '(') {
    error.SetErrorStringWithFormat("'(' expected");
    return false;
  }
  Advance();

  if (CurrentToken().kind != ')') {
    if (!TypeList(type_list, error)) {
      return false;
    }
  }

  if (CurrentToken().kind != ')') {
    error.SetErrorStringWithFormat("')' expected");
    return false;
  }
  Advance();

  return true;
}

bool Parser::BracketTypeList(std::vector<RustTypeExpressionUP> *type_list, Status &error) {
  if (CurrentToken().kind != '<') {
    error.SetErrorStringWithFormat("'<' expected");
    return false;
  }
  Advance();

  if (CurrentToken().kind != '>' && CurrentToken().kind != RSH) {
    if (!TypeList(type_list, error)) {
      return false;
    }
  }

  if (CurrentToken().kind == RSH) {
    ReplaceTokenKind('>');
  } else if (CurrentToken().kind != '>') {
    error.SetErrorStringWithFormat("'>' expected");
    return false;
  } else {
    Advance();
  }

  return true;
}

RustTypeExpressionUP Parser::FunctionType(Status &error) {
  assert(CurrentToken().kind == FN);
  Advance();

  std::vector<RustTypeExpressionUP> type_list;
  if (!ParenTypeList(&type_list, error)) {
    return RustTypeExpressionUP();
  }

  if (CurrentToken().kind != ARROW) {
    error.SetErrorString("'->' expected");
    return RustTypeExpressionUP();
  }
  Advance();

  RustTypeExpressionUP return_type = Type(error);
  if (!return_type) {
    return return_type;
  }

  return llvm::make_unique<RustFunctionTypeExpression>(std::move(return_type),
                                                       std::move(type_list));
}

RustTypeExpressionUP Parser::TupleType(Status &error) {
  assert(CurrentToken().kind == '(');
  // Don't advance here, ParenTypeList is going to deal with the open
  // paren.

  std::vector<RustTypeExpressionUP> type_list;
  if (!ParenTypeList(&type_list, error)) {
    return RustTypeExpressionUP();
  }

  return llvm::make_unique<RustTupleTypeExpression>(std::move(type_list));
}

RustTypeExpressionUP Parser::TypePath(Status &error) {
  bool relative = true;
  int supers = 0;

  bool saw_self = CurrentToken().kind == SELF;
  if (saw_self) {
    Advance();
    if (CurrentToken().kind != COLONCOLON) {
      error.SetErrorString("'::' expected");
      return RustTypeExpressionUP();
    }
  }

  if (CurrentToken().kind == COLONCOLON) {
    if (!saw_self) {
      relative = false;
    }
    Advance();
  }

  if (relative) {
    while (CurrentToken().kind == SUPER) {
      ++supers;
      Advance();
      if (CurrentToken().kind != COLONCOLON) {
        error.SetErrorString("'::' expected after 'super'");
        return RustTypeExpressionUP();
      }
      Advance();
    }
  }

  std::vector<std::string> path;
  while (CurrentToken().kind == IDENTIFIER) {
    path.emplace_back(std::move(CurrentToken().str));
    Advance();
    if (CurrentToken().kind != COLONCOLON) {
      break;
    }
    Advance();
    if (CurrentToken().kind != IDENTIFIER && CurrentToken().kind != '<') {
      error.SetErrorString("identifier or '<' expected");
      return RustTypeExpressionUP();
    }
  }

  std::vector<RustTypeExpressionUP> type_list;
  if (CurrentToken().kind == '<') {
    if (!BracketTypeList(&type_list, error)) {
      return RustTypeExpressionUP();
    }
  }

  if (path.empty()) {
    error.SetErrorString("identifier expected");
    return RustTypeExpressionUP();
  }

  RustPathUP name_path = llvm::make_unique<RustPath>(saw_self, relative, supers, std::move(path),
                                                     std::move(type_list));
  return llvm::make_unique<RustPathTypeExpression>(std::move(name_path));
}

RustTypeExpressionUP Parser::Type(Status &error) {
  switch (CurrentToken().kind) {
  case '[':
    return ArrayType(error);

  case '&':
    return ReferenceType(error);

  case '*':
    return PointerType(error);

  case FN:
    return FunctionType(error);

  case '(':
    return TupleType(error);

  case SUPER:
  case SELF:
  case IDENTIFIER:
  case COLONCOLON:
    return TypePath(error);

  default:
    error.SetErrorString("expected type");
    return RustTypeExpressionUP();
  }
}
