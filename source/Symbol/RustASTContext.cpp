//===-- RustASTContext.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <mutex>
#include <utility>
#include <vector>

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/UniqueCStringMap.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/DataFormatters/StringPrinter.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Symbol/RustASTContext.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Core/DumpDataExtractor.h"

#include "llvm/Support/Threading.h"

#include "Plugins/ExpressionParser/Rust/RustUserExpression.h"
#include "Plugins/SymbolFile/DWARF/DWARFASTParserRust.h"

#include <unordered_map>

using namespace lldb;

namespace lldb_private {

class RustAggregateBase;
class RustArray;
class RustBool;
class RustCLikeEnum;
class RustEnum;
class RustFunction;
class RustIntegral;
class RustPointer;
class RustStruct;
class RustTuple;
class RustTypedef;

class RustType {
protected:

  RustType(const ConstString &name) : m_name(name) {}
  DISALLOW_COPY_AND_ASSIGN (RustType);

public:

  virtual ~RustType() {}

  ConstString Name() const { return m_name; }

  virtual lldb::Format Format() const {
    return eFormatBytes;
  }

  virtual std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                             const std::string &varname) = 0;

  virtual uint32_t TypeInfo(CompilerType *element_type) const = 0;
  virtual lldb::TypeClass TypeClass() const = 0;
  virtual uint64_t ByteSize() const = 0;

  virtual RustAggregateBase *AsAggregate() { return nullptr; }
  virtual RustArray *AsArray() { return nullptr; }
  virtual RustBool *AsBool() { return nullptr; }
  virtual RustCLikeEnum *AsCLikeEnum() { return nullptr; }
  virtual RustEnum *AsEnum() { return nullptr; }
  virtual RustFunction *AsFunction() { return nullptr; }
  virtual RustIntegral *AsInteger () { return nullptr; }
  virtual RustPointer *AsPointer () { return nullptr; }
  virtual RustTuple *AsTuple() { return nullptr; }
  virtual RustTypedef *AsTypedef() { return nullptr; }

  virtual bool IsAggregateType() const { return false; }
  virtual bool IsCharType() const { return false; }
  virtual bool IsFloatType() const { return false; }

private:
  ConstString m_name;
};

class RustBool : public RustType {
public:
  RustBool(const ConstString &name) : RustType(name) {}
  DISALLOW_COPY_AND_ASSIGN(RustBool);

  RustBool *AsBool() override {
    return this;
  }

  lldb::Format Format() const override {
    return eFormatBoolean;
  }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassBuiltin;
  }

  uint64_t ByteSize() const override {
    return 1;
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    return "bool " + varname;
  }
};

class RustIntegral : public RustType {
public:
  RustIntegral(const ConstString &name, bool is_signed, uint64_t byte_size,
               bool is_char = false)
    : RustType(name),
      m_is_signed(is_signed),
      m_byte_size(byte_size),
      m_is_char(is_char)
  {}
  DISALLOW_COPY_AND_ASSIGN(RustIntegral);

  lldb::Format Format() const override {
    if (m_is_char)
      return eFormatUnicode32;
    return m_is_signed ? eFormatDecimal : eFormatUnsigned;
  }

  bool IsSigned() const { return m_is_signed; }
  uint64_t ByteSize() const override { return m_byte_size; }

  RustIntegral *AsInteger () override { return this; }

  bool IsCharType() const override { return m_is_char; }

  uint32_t TypeInfo(CompilerType *) const override {
    uint32_t result = eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar | eTypeIsInteger;
    if (m_is_signed)
      result |= eTypeIsSigned;
    return result;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassBuiltin;
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    // These names are predefined by clang.
    std::string result = "__";
    if (!m_is_signed) {
      result += "U";
    }
    result += "INT" + std::to_string(8 * m_byte_size) + "_TYPE__ " + varname;
    return result;
  }

private:

  bool m_is_signed;
  uint64_t m_byte_size;
  bool m_is_char;
};

class RustCLikeEnum : public RustType {
public:
  RustCLikeEnum(const ConstString &name, const CompilerType &underlying_type,
                std::map<uint64_t, std::string> &&values)
    : RustType(name),
      m_underlying_type(underlying_type),
      m_values(std::move(values))
  {
  }
  DISALLOW_COPY_AND_ASSIGN(RustCLikeEnum);

  RustCLikeEnum *AsCLikeEnum() override { return this; }

  lldb::Format Format() const override {
    return eFormatEnum;
  }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeHasValue | eTypeIsEnumeration | eTypeIsScalar;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassEnumeration;
  }

  uint64_t ByteSize() const override {
    return m_underlying_type.GetByteSize(nullptr);
  }

  bool IsSigned() const {
    bool is_signed;
    return m_underlying_type.IsIntegerType(is_signed) && is_signed;
  }

  bool FindName(uint64_t val, std::string &name) {
    auto iter = m_values.find(val);
    if (iter == m_values.end()) {
      return false;
    }
    name = iter->second;
    return true;
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    RustType *type = (RustType *) m_underlying_type.GetOpaqueQualType();
    return type->GetCABITypeDeclaration(name_map, varname);
  }

private:

  CompilerType m_underlying_type;
  std::map<uint64_t, std::string> m_values;
};

class RustFloat : public RustType {
public:
  RustFloat(const ConstString &name, uint64_t byte_size)
    : RustType(name),
      m_byte_size(byte_size)
  {}
  DISALLOW_COPY_AND_ASSIGN(RustFloat);

  lldb::Format Format() const override {
    return eFormatFloat;
  }

  bool IsFloatType() const override { return true; }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeIsBuiltIn | eTypeHasValue | eTypeIsFloat;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassBuiltin;
  }

  uint64_t ByteSize() const override { return m_byte_size; }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    return (m_byte_size == 4 ? "float " : "double ") + varname;
  }

private:

  uint64_t m_byte_size;
};

class RustPointer : public RustType {
public:
  // Pointers and references are handled similarly.
  RustPointer(const ConstString &name, const CompilerType &pointee, uint64_t byte_size)
    : RustType(name),
      m_pointee(pointee),
      m_byte_size(byte_size)
  {}
  DISALLOW_COPY_AND_ASSIGN(RustPointer);

  lldb::Format Format() const override {
    return eFormatPointer;
  }

  CompilerType PointeeType() const { return m_pointee; }

  RustPointer *AsPointer() override { return this; }

  uint32_t TypeInfo(CompilerType *elem) const override {
    if (elem)
      *elem = m_pointee;
    return eTypeIsBuiltIn | eTypeHasValue | eTypeIsPointer;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassPointer;
  }

  uint64_t ByteSize() const override {
    return m_byte_size;
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    RustType *p_type = (RustType *) m_pointee.GetOpaqueQualType();
    if (p_type->AsFunction()) {
      // This does the right thing, see the implementation.
      return p_type->GetCABITypeDeclaration(name_map, varname);
    }
    return p_type->GetCABITypeDeclaration(name_map, "") + "* " + varname;
  }

private:

  CompilerType m_pointee;
  uint64_t m_byte_size;
};

class RustArray : public RustType {
public:
  RustArray(const ConstString &name, uint64_t length, const CompilerType &elem)
    : RustType(name),
      m_length(length),
      m_elem(elem)
  {}
  DISALLOW_COPY_AND_ASSIGN(RustArray);

  uint64_t Length() const { return m_length; }
  RustArray *AsArray() override { return this; }
  CompilerType ElementType() const { return m_elem; }
  bool IsAggregateType() const override { return true; }

  uint32_t TypeInfo(CompilerType *elem) const override {
    if (elem)
      *elem = m_elem;
    return eTypeHasChildren | eTypeIsArray;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassArray;
  }

  uint64_t ByteSize() const override {
    return m_elem.GetByteSize(nullptr) * m_length;
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    RustType *type = (RustType *) m_elem.GetOpaqueQualType();
    return type->GetCABITypeDeclaration(name_map, varname)
      + "[" + std::to_string(m_length) + "]";
  }

private:
  uint64_t m_length;
  CompilerType m_elem;
};

// Base type for struct, tuple, and tuple struct.
class RustAggregateBase : public RustType {
protected:
  RustAggregateBase(const ConstString &name, uint64_t byte_size, bool has_discriminant = false)
    : RustType(name),
      m_byte_size(byte_size),
      m_has_discriminant(has_discriminant)
  {}

  DISALLOW_COPY_AND_ASSIGN(RustAggregateBase);

public:

  RustAggregateBase *AsAggregate() override { return this; }

  bool IsAggregateType() const override { return true; }

  size_t FieldCount() const { return m_fields.size(); }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeHasChildren | eTypeIsStructUnion;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassStruct;
  }

  uint64_t ByteSize() const override {
    return m_byte_size;
  }

  struct Field {
    Field(const ConstString &name, const CompilerType &type, uint64_t offset)
      : m_name(name),
        m_type(type),
        m_offset(offset)
    {
    }

    ConstString m_name;
    CompilerType m_type;
    uint64_t m_offset;
  };

  void AddField(const ConstString &name, const CompilerType &type, uint64_t offset) {
    m_fields.emplace_back(name, type, offset);
  }

  virtual void FinishInitialization() {
  }

  bool HasDiscriminant() const {
    return m_has_discriminant;
  }

  // With the old-style enum encoding, after the discriminant's
  // location is computed the member types no longer need to have
  // theirs, so they are dropped.
  void DropDiscriminant() {
    if (m_has_discriminant) {
      m_has_discriminant = false;
      m_fields.erase(m_fields.begin());
    }
  }

  const Field *FieldAt(size_t idx) {
    if (idx >= m_fields.size())
      return nullptr;
    return &m_fields[idx];
  }

  typedef std::vector<Field>::const_iterator const_iterator;

  const_iterator begin() const {
    return m_fields.begin();
  }

  const_iterator end() const {
    return m_fields.end();
  }

  // Type-printing support.
  virtual const char *Tag() const = 0;

  virtual const char *TagName() const {
    return Name().AsCString();
  }

  virtual const char *Opener() const = 0;
  virtual const char *Closer() const = 0;

protected:

  std::string GetFieldsCABITypeDeclaration(RustASTContext::TypeNameMap *name_map) {
    int argno = 0;
    std::string result;
    for (const Field &f : m_fields) {
      RustType *rtype = static_cast<RustType *>(f.m_type.GetOpaqueQualType());
      std::string name;
      if (f.m_name.IsEmpty()) {
        name = "__" + std::to_string(argno++);
      } else {
        name = f.m_name.AsCString();
      }
      result += rtype->GetCABITypeDeclaration(name_map, name) + "; ";
    }
    return result;
  }

private:

  uint64_t m_byte_size;
  std::vector<Field> m_fields;
  bool m_has_discriminant;
};

class RustTuple : public RustAggregateBase {
public:
  RustTuple(const ConstString &name, uint64_t byte_size, bool has_discriminant)
    : RustAggregateBase(name, byte_size, has_discriminant)
  {}

  DISALLOW_COPY_AND_ASSIGN(RustTuple);

  RustTuple *AsTuple() override { return this; }

  void AddField(const CompilerType &type, uint64_t offset) {
    RustAggregateBase::AddField(ConstString(), type, offset);
  }

  const char *Tag() const override {
    return IsTuple() ? "" : "struct ";
  }
  const char *TagName() const override {
    if (IsTuple()) {
      return "";
    }
    return Name().AsCString();
  }
  const char *Opener() const override {
    return "(";
  }
  const char *Closer() const override {
    return ")";
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    std::string tagname;
    if (name_map->Tag(this, &tagname)) {
      std::string def = "  struct " + tagname + "{"
        + GetFieldsCABITypeDeclaration(name_map) + " };\n";
      name_map->typedefs.append(def);
    }
    return tagname + " " + varname;
  }

private:

  // As opposed to a tuple struct.
  bool IsTuple() const {
    ConstString name = Name();
    // For the time being we must examine the name, because the DWARF
    // doesn't provide anything else.
    return name.IsEmpty() || name.AsCString()[0] == '(';
  }
};

class RustStruct : public RustAggregateBase {
public:
  RustStruct(const ConstString &name, uint64_t byte_size, bool has_discriminant)
    : RustAggregateBase(name, byte_size, has_discriminant)
  {}

  DISALLOW_COPY_AND_ASSIGN(RustStruct);

  const char *Tag() const override {
    return "struct ";
  }
  const char *Opener() const override {
    return "{";
  }
  const char *Closer() const override {
    return "}";
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    std::string tagname;
    if (name_map->Tag(this, &tagname)) {
      std::string def = "  struct " + tagname + "{"
        + GetFieldsCABITypeDeclaration(name_map) + " };\n";
      name_map->typedefs.append(def);
    }
    return tagname + " " + varname;
  }
};

class RustUnion : public RustAggregateBase {
public:
  RustUnion(const ConstString &name, uint64_t byte_size)
    : RustAggregateBase(name, byte_size)
  {}

  DISALLOW_COPY_AND_ASSIGN(RustUnion);

  const char *Tag() const override {
    return "union ";
  }
  const char *Opener() const override {
    return "{";
  }
  const char *Closer() const override {
    return "}";
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    std::string tagname;
    if (name_map->Tag(this, &tagname)) {
      std::string def = "  union " + tagname + "{"
        + GetFieldsCABITypeDeclaration(name_map) + " };\n";
      name_map->typedefs.append(def);
    }
    return tagname + " " + varname;
  }
};

// A Rust enum, not a C-like enum.
class RustEnum : public RustAggregateBase {
public:
  RustEnum(const ConstString &name, uint64_t byte_size,
           uint32_t discr_offset, uint32_t discr_byte_size)
    : RustAggregateBase(name, byte_size),
      m_discr_offset(discr_offset),
      m_discr_byte_size(discr_byte_size),
      m_default(-1)
  {}

  DISALLOW_COPY_AND_ASSIGN(RustEnum);

  RustEnum *AsEnum() override { return this; }

  const char *Tag() const override {
    return "enum ";
  }
  const char *Opener() const override {
    return "{";
  }
  const char *Closer() const override {
    return "}";
  }

  // Record the discriminant for the most recently added field.
  void RecordDiscriminant(bool is_default, uint64_t discriminant) {
    int value = int(FieldCount() - 1);
    if (is_default) {
      m_default = value;
    } else {
      m_discriminants[discriminant] = value;
    }
  }

  void GetDiscriminantLocation(uint64_t &discr_offset, uint64_t &discr_byte_size) {
    discr_offset = m_discr_offset;
    discr_byte_size = m_discr_byte_size;
  }

  CompilerType FindEnumVariant(uint64_t discriminant) {
    auto iter = m_discriminants.find(discriminant);
    int idx = m_default;
    if (iter != m_discriminants.end()) {
      idx = iter->second;
    }
    return FieldAt(idx)->m_type;
  }

  void FinishInitialization() override {
    for (auto&& iter : *this) {
      RustType *rtype = static_cast<RustType *>(iter.m_type.GetOpaqueQualType());
      if (RustAggregateBase* agg = rtype->AsAggregate()) {
        agg->DropDiscriminant();
      }
    }
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    std::string tagname;
    if (name_map->Tag(this, &tagname)) {
      std::string def = "struct " + tagname + "{ ";
      // If the discriminant comes first, then it is a hidden field,
      // which we'll emit.  Otherwise, it is in a hole somewhere, or
      // perhaps overlaid with some other field, so we don't bother.
      // (This is unwarranted compiler knowledge - FIXME.)  If there are
      // zero or one fields then there is no discriminant.
      if (FieldCount() > 1 && m_discr_offset == 0) {
        def += "int" + std::to_string(8 * m_discr_byte_size) + "_t __discr; ";
      }
      def += GetFieldsCABITypeDeclaration(name_map) + " };\n";
      name_map->typedefs.append(def);
    }
    return tagname + " " + varname;
  }

private:

  // The offset and byte size of the discriminant.  Note that, as a
  // special case, if there is only a single field then the
  // discriminant will be assumed not to exist.
  uint32_t m_discr_offset;
  uint32_t m_discr_byte_size;

  // The index in m_fields of the default variant.  -1 if there is no
  // default variant.
  int m_default;

  // This maps from discriminant values to indices in m_fields.  This
  // is used to find the correct variant given a discriminant value.
  std::unordered_map<uint64_t, int> m_discriminants;
};

class RustFunction : public RustType {
public:
  RustFunction (const ConstString &name, uint64_t byte_size,
                const CompilerType &return_type,
                const std::vector<CompilerType> &&arguments)
    : RustType(name),
      m_byte_size(byte_size),
      m_return_type(return_type),
      m_arguments(std::move(arguments))
  {
  }
  DISALLOW_COPY_AND_ASSIGN(RustFunction);

  // do we care about the names?
  void AddArgument(const CompilerType &type) {
    m_arguments.push_back(type);
  }

  RustFunction *AsFunction() override { return this; }

  CompilerType ReturnType() const { return m_return_type; }
  size_t ArgumentCount() { return m_arguments.size(); }
  CompilerType Argument(size_t i) { return m_arguments[i]; }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeIsFuncPrototype | eTypeHasValue;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassFunction;
  }

  uint64_t ByteSize() const override {
    return m_byte_size;
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    RustType *type = (RustType *) m_return_type.GetOpaqueQualType();

    std::string result = type->GetCABITypeDeclaration(name_map, "") + " (*" +
      varname + ")(";
    bool first = true;
    for (CompilerType &iter : m_arguments) {
      RustType *type = (RustType *) iter.GetOpaqueQualType();
      if (!first) {
        result += ", ";
      }
      first = false;
      result += type->GetCABITypeDeclaration(name_map, "");
    }

    return result + ")";
  }

private:

  uint64_t m_byte_size;
  CompilerType m_return_type;
  std::vector<CompilerType> m_arguments;
};

class RustTypedef : public RustType {
public:

  RustTypedef(const ConstString &name, const CompilerType &type)
    : RustType(name),
      m_type(type)
  {
  }

  DISALLOW_COPY_AND_ASSIGN(RustTypedef);

  RustTypedef *AsTypedef() override { return this; }
  CompilerType UnderlyingType() const { return m_type; }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeIsTypedef;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassTypedef;
  }

  uint64_t ByteSize() const override {
    return m_type.GetByteSize(nullptr);
  }

  std::string GetCABITypeDeclaration(RustASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    RustType *type = (RustType *) m_type.GetOpaqueQualType();
    return type->GetCABITypeDeclaration(name_map, varname);
  }

private:
  CompilerType m_type;
};

class RustDecl;
class RustDeclContext;

class RustDeclBase {
public:

  ConstString Name() const {
    return m_name;
  }

  ConstString QualifiedName() {
    if (!m_parent) {
      return m_name;
    }
    if (!m_full_name) {
      ConstString basename = m_parent->QualifiedName();
      if (basename) {
        std::string qual = std::string(basename.AsCString()) + "::" + m_name.AsCString();
        m_full_name = ConstString(qual.c_str());
      } else {
        m_full_name = m_name;
      }
    }
    return m_full_name;
  }

  RustDeclContext *Context() const {
    // Always succeeds.
    return m_parent->AsDeclContext();
  }

  virtual RustDecl *AsDecl() { return nullptr; }
  virtual RustDeclContext *AsDeclContext() { return nullptr; }

  virtual ~RustDeclBase() { }

protected:

  RustDeclBase(const ConstString &name, RustDeclBase *parent)
    : m_name(name),
      m_parent(parent)
  {
  }

private:

  ConstString m_name;
  // This is really a RustDeclContext.
  RustDeclBase *m_parent;
  ConstString m_full_name;
};

class RustDeclContext : public RustDeclBase {
public:
  RustDeclContext(const ConstString &name, RustDeclContext *parent)
    : RustDeclBase(name, parent)
  {
  }

  RustDeclContext *AsDeclContext() override { return this; }

  RustDeclBase *FindByName(const ConstString &name) {
    auto iter = m_decls.find(name);
    if (iter == m_decls.end()) {
      return nullptr;
    }
    return iter->second.get();
  }

  void AddItem(std::unique_ptr<RustDeclBase> &&item) {
    ConstString name = item->Name();
    m_decls[name] = std::move(item);
  }

private:
  std::map<ConstString, std::unique_ptr<RustDeclBase>> m_decls;
};

class RustDecl : public RustDeclBase {
public:
  RustDecl(const ConstString &name, const ConstString &mangled, RustDeclContext *parent)
    : RustDeclBase(name, parent),
      m_mangled(mangled)
  {
    assert(parent);
  }

  RustDecl *AsDecl() override { return this; }

  ConstString MangledName() const {
    return m_mangled;
  }

private:

  ConstString m_mangled;
};

} // namespace lldb_private
using namespace lldb_private;

RustASTContext::RustASTContext()
    : TypeSystem(eKindRust),
      m_pointer_byte_size(0)
{
}

RustASTContext::~RustASTContext() {}

//------------------------------------------------------------------
// PluginInterface functions
//------------------------------------------------------------------

ConstString RustASTContext::GetPluginNameStatic() {
  return ConstString("rust");
}

ConstString RustASTContext::GetPluginName() {
  return RustASTContext::GetPluginNameStatic();
}

uint32_t RustASTContext::GetPluginVersion() {
  return 1;
}

lldb::TypeSystemSP RustASTContext::CreateInstance(lldb::LanguageType language,
                                                  Module *module,
                                                  Target *target) {
  if (language == eLanguageTypeRust) {
    ArchSpec arch;
    std::shared_ptr<RustASTContext> astc;
    if (module) {
      arch = module->GetArchitecture();
      astc = std::shared_ptr<RustASTContext>(new RustASTContext);
    } else if (target) {
      arch = target->GetArchitecture();
      astc = std::shared_ptr<RustASTContextForExpr>(
          new RustASTContextForExpr(target->shared_from_this()));
    }

    if (arch.IsValid()) {
      astc->SetAddressByteSize(arch.GetAddressByteSize());
      return astc;
    }
  }
  return lldb::TypeSystemSP();
}

void RustASTContext::EnumerateSupportedLanguages(
    std::set<lldb::LanguageType> &languages_for_types,
    std::set<lldb::LanguageType> &languages_for_expressions) {
  static std::vector<lldb::LanguageType> s_supported_languages_for_types(
      {lldb::eLanguageTypeRust});

  static std::vector<lldb::LanguageType> s_supported_languages_for_expressions(
      {});

  languages_for_types.insert(s_supported_languages_for_types.begin(),
                             s_supported_languages_for_types.end());
  languages_for_expressions.insert(
      s_supported_languages_for_expressions.begin(),
      s_supported_languages_for_expressions.end());
}

void RustASTContext::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(), "Rust AST context plug-in",
                                CreateInstance, EnumerateSupportedLanguages);
}

void RustASTContext::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

//----------------------------------------------------------------------
// Tests
//----------------------------------------------------------------------

bool RustASTContext::IsArrayType(lldb::opaque_compiler_type_t type,
                                 CompilerType *element_type, uint64_t *size,
                                 bool *is_incomplete) {
  if (element_type)
    element_type->Clear();
  if (size)
    *size = 0;
  if (is_incomplete)
    *is_incomplete = false;
  RustArray *array = static_cast<RustType *>(type)->AsArray();
  if (array) {
    if (size)
      *size = array->Length();
    if (element_type)
      *element_type = array->ElementType();
    return true;
  }
  return false;
}

bool RustASTContext::IsVectorType(lldb::opaque_compiler_type_t type,
                                  CompilerType *element_type, uint64_t *size) {
  if (element_type)
    element_type->Clear();
  if (size)
    *size = 0;
  return false;
}

bool RustASTContext::IsAggregateType(lldb::opaque_compiler_type_t type) {
  return static_cast<RustType *>(type)->IsAggregateType();
}

bool RustASTContext::IsBeingDefined(lldb::opaque_compiler_type_t type) {
  return false;
}

bool RustASTContext::IsCharType(lldb::opaque_compiler_type_t type) {
  return static_cast<RustType *>(type)->IsCharType();
}

bool RustASTContext::IsCompleteType(lldb::opaque_compiler_type_t type) {
  return bool(type);
}

bool RustASTContext::IsConst(lldb::opaque_compiler_type_t type) {
  return false;
}

bool RustASTContext::IsCStringType(lldb::opaque_compiler_type_t type,
                                   uint32_t &length) {
  return false;
}

bool RustASTContext::IsDefined(lldb::opaque_compiler_type_t type) {
  return type != nullptr;
}

bool RustASTContext::IsFloatingPointType(lldb::opaque_compiler_type_t type,
                                         uint32_t &count, bool &is_complex) {
  is_complex = false;
  if (static_cast<RustType *>(type)->IsFloatType()) {
    count = 1;
    return true;
  }
  count = 0;
  return false;
}

bool RustASTContext::IsFunctionType(lldb::opaque_compiler_type_t type,
                                    bool *is_variadic_ptr) {
  if (is_variadic_ptr)
    *is_variadic_ptr = false;
  return static_cast<RustType *>(type)->AsFunction() != nullptr;
}

uint32_t RustASTContext::IsHomogeneousAggregate(lldb::opaque_compiler_type_t type,
                                                CompilerType *base_type_ptr) {
  // FIXME should detect "homogeneous floating-point aggregates".
  return false;
}

size_t
RustASTContext::GetNumberOfFunctionArguments(lldb::opaque_compiler_type_t type) {
  RustFunction *func = static_cast<RustType *>(type)->AsFunction();
  if (func) {
    return func->ArgumentCount();
  }
  return -1;
}

CompilerType
RustASTContext::GetFunctionArgumentAtIndex(lldb::opaque_compiler_type_t type,
                                           const size_t index) {
  RustFunction *func = static_cast<RustType *>(type)->AsFunction();
  if (func) {
    return func->Argument(index);
  }
  return CompilerType();
}

bool RustASTContext::IsFunctionPointerType(lldb::opaque_compiler_type_t type) {
  CompilerType pointee;
  if (!IsPointerType(type, &pointee)) {
    return false;
  }
  return pointee.IsFunctionType();
}

bool RustASTContext::IsBlockPointerType(lldb::opaque_compiler_type_t type,
                                        CompilerType *function_pointer_type_ptr) {
  return false;
}

bool RustASTContext::IsIntegerType(lldb::opaque_compiler_type_t type,
                                   bool &is_signed) {
  if (!type)
    return false;

  RustIntegral *inttype = static_cast<RustType *>(type)->AsInteger();
  if (inttype) {
    is_signed = inttype->IsSigned();
    return true;
  }
  return false;
}

bool RustASTContext::IsPolymorphicClass(lldb::opaque_compiler_type_t type) {
  return false;
}

bool RustASTContext::IsPossibleDynamicType(lldb::opaque_compiler_type_t type,
                                           CompilerType *target_type, // Can pass NULL
                                           bool check_cplusplus, bool check_objc) {
  if (target_type)
    target_type->Clear();
  // FIXME eventually we'll handle trait object pointers here
  if (static_cast<RustType *>(type)->AsEnum()) {
    return true;
  }
  return false;
}

bool RustASTContext::IsRuntimeGeneratedType(lldb::opaque_compiler_type_t type) {
  return false;
}

bool RustASTContext::IsPointerType(lldb::opaque_compiler_type_t type,
                                   CompilerType *pointee_type) {
  if (!type)
    return false;
  if (RustPointer *ptr = static_cast<RustType *>(type)->AsPointer()) {
    if (pointee_type)
      *pointee_type = ptr->PointeeType();
    return true;
  }
  return false;
}

bool RustASTContext::IsPointerOrReferenceType(lldb::opaque_compiler_type_t type,
                                              CompilerType *pointee_type) {
  return IsPointerType(type, pointee_type);
}

bool RustASTContext::IsReferenceType(lldb::opaque_compiler_type_t type,
                                     CompilerType *pointee_type,
                                     bool *is_rvalue) {
  return false;
}

bool RustASTContext::IsScalarType(lldb::opaque_compiler_type_t type) {
  return !IsAggregateType(type);
}

bool RustASTContext::IsTypedefType(lldb::opaque_compiler_type_t type) {
  if (type)
    return static_cast<RustType *>(type)->AsTypedef() != nullptr;
  return false;
}

bool RustASTContext::IsBooleanType(lldb::opaque_compiler_type_t type) {
  if (type)
    return static_cast<RustType *>(type)->AsBool() != nullptr;
  return false;
}

bool RustASTContext::IsVoidType(lldb::opaque_compiler_type_t type) {
  if (!type)
    return false;
  RustTuple *tuple = static_cast<RustType *>(type)->AsTuple();
  return tuple && !tuple->Name().IsEmpty() &&
    strcmp(tuple->Name().AsCString(), "()") == 0 && tuple->FieldCount() == 0;
}

bool RustASTContext::SupportsLanguage(lldb::LanguageType language) {
  return language == eLanguageTypeRust;
}

//----------------------------------------------------------------------
// Type Completion
//----------------------------------------------------------------------

bool RustASTContext::GetCompleteType(lldb::opaque_compiler_type_t type) {
  return bool(type);
}

//----------------------------------------------------------------------
// AST related queries
//----------------------------------------------------------------------

uint32_t RustASTContext::GetPointerByteSize() {
  return m_pointer_byte_size;
}

//----------------------------------------------------------------------
// Accessors
//----------------------------------------------------------------------

ConstString RustASTContext::GetTypeName(lldb::opaque_compiler_type_t type) {
  if (type)
    return static_cast<RustType *>(type)->Name();
  return ConstString();
}

uint32_t
RustASTContext::GetTypeInfo(lldb::opaque_compiler_type_t type,
                            CompilerType *pointee_or_element_compiler_type) {
  if (pointee_or_element_compiler_type)
    pointee_or_element_compiler_type->Clear();
  if (!type)
    return 0;
  return static_cast<RustType *>(type)->TypeInfo(pointee_or_element_compiler_type);
}

lldb::TypeClass RustASTContext::GetTypeClass(lldb::opaque_compiler_type_t type) {
  if (!type)
    return eTypeClassInvalid;
  return static_cast<RustType *>(type)->TypeClass();
}

lldb::BasicType
RustASTContext::GetBasicTypeEnumeration(lldb::opaque_compiler_type_t type) {
  ConstString name = GetTypeName(type);
  if (name.IsEmpty()) {
    // Nothing.
  } else if (strcmp(name.AsCString(), "()") == 0) {
    return eBasicTypeVoid;
  } else if (strcmp(name.AsCString(), "bool") == 0) {
    return eBasicTypeBool;
  }
  return eBasicTypeInvalid;
}

lldb::LanguageType
RustASTContext::GetMinimumLanguage(lldb::opaque_compiler_type_t type) {
  return lldb::eLanguageTypeRust;
}

unsigned RustASTContext::GetTypeQualifiers(lldb::opaque_compiler_type_t type) {
  return 0;
}

//----------------------------------------------------------------------
// Creating related types
//----------------------------------------------------------------------

CompilerType
RustASTContext::GetArrayElementType(lldb::opaque_compiler_type_t type,
                                    uint64_t *stride) {
  RustArray *array = static_cast<RustType *>(type)->AsArray();
  if (array) {
    if (stride) {
      *stride = array->ElementType().GetByteSize(nullptr);
    }
    return array->ElementType();
  }
  return CompilerType();
}

CompilerType RustASTContext::GetCanonicalType(lldb::opaque_compiler_type_t type) {
  RustTypedef *t = static_cast<RustType *>(type)->AsTypedef();
  if (t)
    return t->UnderlyingType();
  return CompilerType(this, type);
}

CompilerType
RustASTContext::GetFullyUnqualifiedType(lldb::opaque_compiler_type_t type) {
  return CompilerType(this, type);
}

// Returns -1 if this isn't a function or if the function doesn't have a
// prototype.
// Returns a value >= 0 if there is a prototype.
int RustASTContext::GetFunctionArgumentCount(lldb::opaque_compiler_type_t type) {
  return GetNumberOfFunctionArguments(type);
}

CompilerType
RustASTContext::GetFunctionArgumentTypeAtIndex(lldb::opaque_compiler_type_t type,
                                               size_t idx) {
  return GetFunctionArgumentAtIndex(type, idx);
}

CompilerType
RustASTContext::GetFunctionReturnType(lldb::opaque_compiler_type_t type) {
  if (type) {
    RustFunction *t = static_cast<RustType *>(type)->AsFunction();
    if (t) {
      return t->ReturnType();
    }
  }
  return CompilerType();
}

size_t RustASTContext::GetNumMemberFunctions(lldb::opaque_compiler_type_t type) {
  return 0;
}

TypeMemberFunctionImpl
RustASTContext::GetMemberFunctionAtIndex(lldb::opaque_compiler_type_t type,
                                         size_t idx) {
  return TypeMemberFunctionImpl();
}

CompilerType
RustASTContext::GetNonReferenceType(lldb::opaque_compiler_type_t type) {
  return CompilerType(this, type);
}

CompilerType RustASTContext::GetPointeeType(lldb::opaque_compiler_type_t type) {
  if (!type)
    return CompilerType();
  RustPointer *p = static_cast<RustType *>(type)->AsPointer();
  if (p)
    return p->PointeeType();
  return CompilerType();
}

CompilerType RustASTContext::GetPointerType(lldb::opaque_compiler_type_t type) {
  ConstString type_name = GetTypeName(type);
  // Arbitrarily look for a raw pointer here.
  ConstString pointer_name(std::string("*mut ") + type_name.GetCString());
  return CreatePointerType(pointer_name, CompilerType(this, type), m_pointer_byte_size);
}

// If the current object represents a typedef type, get the underlying type
CompilerType RustASTContext::GetTypedefedType(lldb::opaque_compiler_type_t type) {
  if (type) {
    RustTypedef *t = static_cast<RustType *>(type)->AsTypedef();
    if (t)
      return t->UnderlyingType();
  }
  return CompilerType();
}

//----------------------------------------------------------------------
// Create related types using the current type's AST
//----------------------------------------------------------------------
CompilerType RustASTContext::GetBasicTypeFromAST(lldb::BasicType basic_type) {
  return CompilerType();
}

CompilerType
RustASTContext::GetBuiltinTypeForEncodingAndBitSize(lldb::Encoding encoding,
                                                    size_t bit_size) {
  return CompilerType();
}

//----------------------------------------------------------------------
// Exploring the type
//----------------------------------------------------------------------

uint64_t RustASTContext::GetBitSize(lldb::opaque_compiler_type_t type,
                                    ExecutionContextScope *exe_scope) {
  if (!type)
    return 0;
  RustType *t = static_cast<RustType *>(type);
  return t->ByteSize() * 8;
}

lldb::Encoding RustASTContext::GetEncoding(lldb::opaque_compiler_type_t type,
                                           uint64_t &count) {
  count = 1;
  bool is_signed;
  if (IsIntegerType(type, is_signed)) {
    return is_signed ? eEncodingSint : eEncodingUint;
  }
  if (IsBooleanType(type)) {
    return eEncodingUint;
  }
  bool is_complex;
  uint32_t complex_count;
  if (IsFloatingPointType(type, complex_count, is_complex)) {
    count = complex_count;
    return eEncodingIEEE754;
  }
  if (IsPointerType(type))
    return eEncodingUint;
  return eEncodingInvalid;
}

lldb::Format RustASTContext::GetFormat(lldb::opaque_compiler_type_t type) {
  if (!type)
    return eFormatDefault;
  return static_cast<RustType *>(type)->Format();
}

size_t RustASTContext::GetTypeBitAlign(lldb::opaque_compiler_type_t type) {
  return 0;
}

uint32_t RustASTContext::GetNumChildren(lldb::opaque_compiler_type_t type,
                                        bool omit_empty_base_classes) {
  if (!type)
    return 0;

  RustType *t = static_cast<RustType *>(type);
  uint32_t result = 0;
  if (RustPointer *ptr = t->AsPointer()) {
    result = ptr->PointeeType().GetNumChildren(omit_empty_base_classes);
    // If the pointee is not an aggregate, return 1 because the
    // pointer has a child.  Not totally sure this makes sense.
    if (result == 0)
      result = 1;
  } else if (RustArray *array = t->AsArray()) {
    result = array->Length();
  } else if (RustTypedef *typ = t->AsTypedef()) {
    result = typ->UnderlyingType().GetNumChildren(omit_empty_base_classes);
  } else if (RustAggregateBase *agg = t->AsAggregate()) {
    result = agg->FieldCount();
  }

  return result;
}

uint32_t RustASTContext::GetNumFields(lldb::opaque_compiler_type_t type) {
  if (!type)
    return 0;
  RustType *t = static_cast<RustType *>(type);
  if (RustTypedef *tdef = t->AsTypedef())
    return tdef->UnderlyingType().GetNumFields();
  if (RustAggregateBase *a = t->AsAggregate())
    return a->FieldCount();
  return 0;
}

CompilerType RustASTContext::GetFieldAtIndex(lldb::opaque_compiler_type_t type,
                                             size_t idx, std::string &name,
                                             uint64_t *bit_offset_ptr,
                                             uint32_t *bitfield_bit_size_ptr,
                                             bool *is_bitfield_ptr) {
  if (bit_offset_ptr)
    *bit_offset_ptr = 0;
  if (bitfield_bit_size_ptr)
    *bitfield_bit_size_ptr = 0;
  if (is_bitfield_ptr)
    *is_bitfield_ptr = false;

  if (!type || !GetCompleteType(type))
    return CompilerType();

  RustType *t = static_cast<RustType *>(type);
  if (RustTypedef *typ = t->AsTypedef())
    return typ->UnderlyingType().GetFieldAtIndex(
        idx, name, bit_offset_ptr, bitfield_bit_size_ptr, is_bitfield_ptr);

  if (RustAggregateBase *s = t->AsAggregate()) {
    const auto *field = s->FieldAt(idx);
    if (field) {
      name = field->m_name.GetStringRef();
      if (bit_offset_ptr)
        *bit_offset_ptr = field->m_offset * 8;
      return field->m_type;
    }
  }
  return CompilerType();
}

CompilerType RustASTContext::GetChildCompilerTypeAtIndex(
    lldb::opaque_compiler_type_t type, ExecutionContext *exe_ctx, size_t idx,
    bool transparent_pointers, bool omit_empty_base_classes,
    bool ignore_array_bounds, std::string &child_name,
    uint32_t &child_byte_size, int32_t &child_byte_offset,
    uint32_t &child_bitfield_bit_size, uint32_t &child_bitfield_bit_offset,
    bool &child_is_base_class, bool &child_is_deref_of_parent,
    ValueObject *valobj, uint64_t &language_flags) {
  child_name.clear();
  child_byte_size = 0;
  child_byte_offset = 0;
  child_bitfield_bit_size = 0;
  child_bitfield_bit_offset = 0;
  child_is_base_class = false;
  child_is_deref_of_parent = false;
  language_flags = 0;

  if (!type || !GetCompleteType(type))
    return CompilerType();

  RustType *t = static_cast<RustType *>(type);
  if (t->AsAggregate()) {
    uint64_t bit_offset;
    CompilerType ret =
        GetFieldAtIndex(type, idx, child_name, &bit_offset, nullptr, nullptr);
    child_byte_size = ret.GetByteSize(
        exe_ctx ? exe_ctx->GetBestExecutionContextScope() : nullptr);
    child_byte_offset = bit_offset / 8;
    return ret;
  } else if (RustPointer *ptr = t->AsPointer()) {
    CompilerType pointee = ptr->PointeeType();
    if (!pointee.IsValid() || pointee.IsVoidType())
      return CompilerType();
    if (transparent_pointers && pointee.IsAggregateType()) {
      bool tmp_child_is_deref_of_parent = false;
      return pointee.GetChildCompilerTypeAtIndex(
          exe_ctx, idx, transparent_pointers, omit_empty_base_classes,
          ignore_array_bounds, child_name, child_byte_size, child_byte_offset,
          child_bitfield_bit_size, child_bitfield_bit_offset,
          child_is_base_class, tmp_child_is_deref_of_parent, valobj,
          language_flags);
    } else {
      child_is_deref_of_parent = true;
      const char *parent_name = valobj ? valobj->GetName().GetCString() : NULL;
      if (parent_name) {
        child_name.assign(1, '*');
        child_name += parent_name;
      }

      // We have a pointer to an simple type
      if (idx == 0 && pointee.GetCompleteType()) {
        child_byte_size = pointee.GetByteSize(
            exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL);
        child_byte_offset = 0;
        return pointee;
      }
    }
  } else if (RustArray *a = t->AsArray()) {
    if (ignore_array_bounds || idx < a->Length()) {
      CompilerType element_type = a->ElementType();
      if (element_type.GetCompleteType()) {
        char element_name[64];
        ::snprintf(element_name, sizeof(element_name), "[%zu]", idx);
        child_name.assign(element_name);
        child_byte_size = element_type.GetByteSize(
            exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL);
        child_byte_offset = (int32_t)idx * (int32_t)child_byte_size;
        return element_type;
      }
    }
  } else if (RustTypedef *typ = t->AsTypedef()) {
    return typ->UnderlyingType().GetChildCompilerTypeAtIndex(
        exe_ctx, idx, transparent_pointers, omit_empty_base_classes,
        ignore_array_bounds, child_name, child_byte_size, child_byte_offset,
        child_bitfield_bit_size, child_bitfield_bit_offset, child_is_base_class,
        child_is_deref_of_parent, valobj, language_flags);
  }
  return CompilerType();
}

// Lookup a child given a name. This function will match base class names
// and member member names in "clang_type" only, not descendants.
uint32_t
RustASTContext::GetIndexOfChildWithName(lldb::opaque_compiler_type_t type,
                                        const char *name,
                                        bool omit_empty_base_classes) {
  if (!type || !GetCompleteType(type))
    return UINT_MAX;

  RustType *t = static_cast<RustType *>(type);
  if (RustAggregateBase *agg = t->AsAggregate()) {
    for (uint32_t i = 0; i < agg->FieldCount(); ++i) {
      const RustAggregateBase::Field *f = agg->FieldAt(i);
      if (f->m_name.GetStringRef() == name)
        return i;
    }
  } else if (RustPointer *typ = t->AsPointer()) {
    return typ->PointeeType().GetIndexOfChildWithName(name, omit_empty_base_classes);
  }
  return UINT_MAX;
}

// Lookup a child member given a name. This function will match member names
// only and will descend into "clang_type" children in search for the first
// member in this class, or any base class that matches "name".
// TODO: Return all matches for a given name by returning a
// vector<vector<uint32_t>>
// so we catch all names that match a given child name, not just the first.
size_t RustASTContext::GetIndexOfChildMemberWithName(
    lldb::opaque_compiler_type_t type, const char *name,
    bool omit_empty_base_classes, std::vector<uint32_t> &child_indexes) {
  uint32_t index = GetIndexOfChildWithName(type, name, omit_empty_base_classes);
  if (index == UINT_MAX)
    return 0;
  child_indexes.push_back(index);
  return 1;
}

// Converts "s" to a floating point value and place resulting floating
// point bytes in the "dst" buffer.
size_t
RustASTContext::ConvertStringToFloatValue(lldb::opaque_compiler_type_t type,
                                        const char *s, uint8_t *dst,
                                        size_t dst_size) {
  assert(false);
  return 0;
}

//----------------------------------------------------------------------
// Dumping types
//----------------------------------------------------------------------
#define DEPTH_INCREMENT 2

void RustASTContext::DumpValue(lldb::opaque_compiler_type_t type,
                               ExecutionContext *exe_ctx, Stream *s,
                               lldb::Format format, const DataExtractor &data,
                               lldb::offset_t data_byte_offset,
                               size_t data_byte_size, uint32_t bitfield_bit_size,
                               uint32_t bitfield_bit_offset, bool show_types,
                               bool show_summary, bool verbose, uint32_t depth) {
  // This doesn't seem to be needed.
  assert(false && "Not implemented");
}

bool RustASTContext::DumpTypeValue(lldb::opaque_compiler_type_t type, Stream *s,
                                   lldb::Format format, const DataExtractor &data,
                                   lldb::offset_t byte_offset, size_t byte_size,
                                   uint32_t bitfield_bit_size,
                                   uint32_t bitfield_bit_offset,
                                   ExecutionContextScope *exe_scope) {
  if (!type)
    return false;
  if (IsAggregateType(type)) {
    return false;
  } else {
    RustType *t = static_cast<RustType *>(type);
    if (RustTypedef *typ = t->AsTypedef()) {
      CompilerType typedef_compiler_type = typ->UnderlyingType();
      if (format == eFormatDefault)
        format = typedef_compiler_type.GetFormat();
      uint64_t typedef_byte_size = typedef_compiler_type.GetByteSize(exe_scope);

      return typedef_compiler_type.DumpTypeValue(
          s,
          format,            // The format with which to display the element
          data,              // Data buffer containing all bytes for this type
          byte_offset,       // Offset into "data" where to grab value from
          typedef_byte_size, // Size of this type in bytes
          bitfield_bit_size, // Size in bits of a bitfield value, if zero don't
                             // treat as a bitfield
          bitfield_bit_offset, // Offset in bits of a bitfield value if
                               // bitfield_bit_size != 0
          exe_scope);
    }

    if (format == eFormatEnum || format == eFormatDefault) {
      if (RustCLikeEnum *clike = t->AsCLikeEnum()) {
        uint64_t value;
        if (clike->IsSigned()) {
          int64_t svalue = data.GetMaxS64Bitfield(&byte_offset, byte_size,
                                                  bitfield_bit_size,
                                                  bitfield_bit_offset);
          value = uint64_t(svalue);
        } else {
          value = data.GetMaxU64Bitfield(&byte_offset, byte_size,
                                         bitfield_bit_size,
                                         bitfield_bit_offset);
        }

        std::string name;
        if (clike->FindName(value, name)) {
          s->Printf("%s::%s", clike->Name().AsCString(), name.c_str());
        } else {
          // If the value couldn't be found, then something went wrong
          // we should inform the user.
          s->Printf("(invalid enum value) %" PRIu64, value);
        }
        return true;
      }
    } else if (format == eFormatUnicode32) {
      if (RustIntegral *intlike = t->AsInteger()) {
        if (intlike->IsCharType()) {
          uint64_t value = data.GetMaxU64Bitfield(&byte_offset, byte_size,
                                                  bitfield_bit_size,
                                                  bitfield_bit_offset);
          switch (value) {
          case '\n':
            s->PutCString("'\\n'");
            break;
          case '\r':
            s->PutCString("'\\r'");
            break;
          case '\t':
            s->PutCString("'\\t'");
            break;
          case '\\':
            s->PutCString("'\\\\'");
            break;
          case '\0':
            s->PutCString("'\\0'");
            break;
          case '\'':
            s->PutCString("'\\''");
            break;

          default:
            if (value < 128 && isprint(value)) {
              s->Printf("'%c'", char(value));
            } else {
              s->Printf("'\\u{%x}'", unsigned(value));
            }
            break;
          }

          return true;
        }
      }
    }

    uint32_t item_count = 1;
    switch (format) {
    default:
    case eFormatBoolean:
    case eFormatBinary:
    case eFormatComplex:
    case eFormatCString:
    case eFormatDecimal:
    case eFormatEnum:
    case eFormatHex:
    case eFormatHexUppercase:
    case eFormatFloat:
    case eFormatOctal:
    case eFormatOSType:
    case eFormatUnsigned:
    case eFormatPointer:
    case eFormatVectorOfChar:
    case eFormatVectorOfSInt8:
    case eFormatVectorOfUInt8:
    case eFormatVectorOfSInt16:
    case eFormatVectorOfUInt16:
    case eFormatVectorOfSInt32:
    case eFormatVectorOfUInt32:
    case eFormatVectorOfSInt64:
    case eFormatVectorOfUInt64:
    case eFormatVectorOfFloat32:
    case eFormatVectorOfFloat64:
    case eFormatVectorOfUInt128:
      break;

    case eFormatChar:
    case eFormatCharPrintable:
    case eFormatCharArray:
    case eFormatBytes:
    case eFormatBytesWithASCII:
      item_count = byte_size;
      byte_size = 1;
      break;

    case eFormatUnicode16:
      item_count = byte_size / 2;
      byte_size = 2;
      break;

    case eFormatUnicode32:
      item_count = byte_size / 4;
      byte_size = 4;
      break;
    }
    return DumpDataExtractor(data, s, byte_offset, format, byte_size, item_count,
                             UINT32_MAX, LLDB_INVALID_ADDRESS, bitfield_bit_size,
                             bitfield_bit_offset, exe_scope);
  }
  return 0;
}

void RustASTContext::DumpSummary(lldb::opaque_compiler_type_t type,
                                 ExecutionContext *exe_ctx, Stream *s,
                                 const DataExtractor &data,
                                 lldb::offset_t data_offset,
                                 size_t data_byte_size) {
  // Apparently there is nothing to do here.
}

void RustASTContext::DumpTypeDescription(lldb::opaque_compiler_type_t type) {
  // Dump to stdout
  StreamFile s(stdout, false);
  DumpTypeDescription(type, &s);
}

void RustASTContext::DumpTypeDescription(lldb::opaque_compiler_type_t type, Stream *s) {
  if (!type)
    return;
  ConstString name = GetTypeName(type);
  RustType *t = static_cast<RustType *>(type);

  if (RustAggregateBase *agg = t->AsAggregate()) {
    s->PutCString(agg->Tag());
    const char *name = agg->TagName();
    s->PutCString(name);
    if (*name) {
      s->PutCString(" ");
    }
    s->PutCString(agg->Opener());
    if (agg->FieldCount() == 0) {
      s->PutCString(agg->Closer());
      return;
    }
    s->IndentMore();
    // A trailing comma looks weird for tuples, so we keep track and
    // don't emit it.
    bool first = true;
    for (auto &&field : *agg) {
      if (!first) {
        s->PutChar(',');
      }
      first = false;
      s->PutChar('\n');
      s->Indent();
      if (!field.m_name.IsEmpty()) {
        s->PutCString(field.m_name.AsCString());
        s->PutCString(": ");
      }
      s->PutCString(field.m_type.GetTypeName().AsCString());
    }
    s->IndentLess();
    s->PutChar('\n');
    s->Indent(agg->Closer());
    return;
  }

  s->PutCString(name.AsCString());
}

RustType *RustASTContext::FindCachedType(const lldb_private::ConstString &name) {
  if (name.IsEmpty())
    return nullptr;
  auto result = m_types.find(name);
  if (result == m_types.end ())
    return nullptr;
  return result->second.get();
}

CompilerType RustASTContext::CacheType(const ConstString &name, RustType *new_type) {
  if (name.IsEmpty()) {
    // Be sure to keep nameless types alive.
    m_anon_types.insert(std::unique_ptr<RustType>(new_type));
  } else {
    m_types[name].reset(new_type);
  }
  return CompilerType(this, new_type);
}

CompilerType RustASTContext::CreateBoolType(const lldb_private::ConstString &name) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustBool(name);
  return CacheType(name, type);
}

CompilerType RustASTContext::CreateIntegralType(const lldb_private::ConstString &name,
                                                bool is_signed,
                                                uint64_t byte_size,
                                                bool is_char_type) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustIntegral(name, is_signed, byte_size, is_char_type);
  return CacheType(name, type);
}

CompilerType RustASTContext::CreateIntrinsicIntegralType(bool is_signed, uint64_t byte_size) {
  char name[100];
  snprintf(name, sizeof(name), "%s%d", is_signed ? "i" : "u", int(byte_size * 8));

  ConstString cname(name);
  return CreateIntegralType(cname, is_signed, byte_size);
}

CompilerType RustASTContext::CreateCharType() {
  ConstString cname("char");
  return CreateIntegralType(cname, false, 4, true);
}

CompilerType RustASTContext::CreateFloatType(const lldb_private::ConstString &name,
                                             uint64_t byte_size) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustFloat(name, byte_size);
  return CacheType(name, type);
}

CompilerType RustASTContext::CreateArrayType(const CompilerType &element_type,
                                             uint64_t length) {
  std::string name = std::string("[") + element_type.GetTypeName().AsCString();
  if (length != 0) {
    name = name + "; " + std::to_string(length);
  }
  name += "]";
  ConstString newname(name);

  if (RustType *cached = FindCachedType(newname))
    return CompilerType(this, cached);
  RustType *type = new RustArray(newname, length, element_type);
  return CacheType(newname, type);
}

CompilerType RustASTContext::CreateTypedefType(const ConstString &name, CompilerType impl) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustTypedef(name, impl);
  return CacheType(name, type);
}

CompilerType
RustASTContext::CreateStructType(const lldb_private::ConstString &name, uint32_t byte_size,
                                 bool has_discriminant) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustStruct(name, byte_size, has_discriminant);
  return CacheType(name, type);
}

CompilerType
RustASTContext::CreateTupleType(const lldb_private::ConstString &name, uint32_t byte_size,
                                bool has_discriminant) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustTuple(name, byte_size, has_discriminant);
  return CacheType(name, type);
}

CompilerType
RustASTContext::CreateUnionType(const lldb_private::ConstString &name, uint32_t byte_size) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustUnion(name, byte_size);
  return CacheType(name, type);
}

CompilerType
RustASTContext::CreatePointerType(const lldb_private::ConstString &name,
                                  const CompilerType &pointee_type,
                                  uint32_t byte_size) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustPointer(name, pointee_type, byte_size);
  return CacheType(name, type);
}

void RustASTContext::AddFieldToStruct(const CompilerType &struct_type,
                                      const lldb_private::ConstString &name,
                                      const CompilerType &field_type,
                                      uint32_t byte_offset,
                                      bool is_default, uint64_t discriminant) {
  if (!struct_type)
    return;
  RustASTContext *ast =
      llvm::dyn_cast_or_null<RustASTContext>(struct_type.GetTypeSystem());
  if (!ast)
    return;
  RustType *type = static_cast<RustType *>(struct_type.GetOpaqueQualType());
  if (RustAggregateBase *a = type->AsAggregate()) {
    a->AddField(name, field_type, byte_offset);
    if (RustEnum *e = type->AsEnum()) {
      e->RecordDiscriminant(is_default, discriminant);
    }
  }
}

CompilerType
RustASTContext::CreateFunctionType(const lldb_private::ConstString &name,
                                   const CompilerType &return_type,
                                   const std::vector<CompilerType> &&params) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustFunction(name, m_pointer_byte_size, return_type, std::move(params));
  return CacheType(name, type);
}

CompilerType
RustASTContext::CreateVoidType() {
  ConstString name("()");
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustTuple(name, 0, false);
  return CacheType(name, type);
}

CompilerType
RustASTContext::CreateEnumType(const lldb_private::ConstString &name,
                               uint64_t byte_size, uint32_t discr_offset,
                               uint32_t discr_byte_size) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustEnum(name, byte_size, discr_offset, discr_byte_size);
  return CacheType(name, type);
}

CompilerType
RustASTContext::CreateCLikeEnumType(const lldb_private::ConstString &name,
                                    const CompilerType &underlying_type,
                                    std::map<uint64_t, std::string> &&values) {
  if (RustType *cached = FindCachedType(name))
    return CompilerType(this, cached);
  RustType *type = new RustCLikeEnum(name, underlying_type, std::move(values));
  return CacheType(name, type);
}

bool
RustASTContext::IsTupleType(const CompilerType &type) {
  if (!type)
    return false;
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());
  if (!ast)
    return false;
  RustType *rtype = static_cast<RustType *>(type.GetOpaqueQualType());
  return bool(rtype->AsTuple());
}

bool
RustASTContext::TypeHasDiscriminant(const CompilerType &type) {
  if (!type)
    return false;
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());
  if (!ast)
    return false;
  RustType *rtype = static_cast<RustType *>(type.GetOpaqueQualType());
  if (RustAggregateBase *a = rtype->AsAggregate())
    return a->HasDiscriminant();
  return false;
}

bool
RustASTContext::GetEnumDiscriminantLocation(const CompilerType &type, uint64_t &discr_offset,
                                            uint64_t &discr_byte_size) {
  if (!type)
    return false;
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());
  if (!ast)
    return false;
  RustType *rtype = static_cast<RustType *>(type.GetOpaqueQualType());
  if (RustEnum *e = rtype->AsEnum()) {
    e->GetDiscriminantLocation(discr_offset, discr_byte_size);
    return true;
  }
  return false;
}

CompilerType
RustASTContext::FindEnumVariant(const CompilerType &type, uint64_t discriminant) {
  if (!type)
    return CompilerType();
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());
  if (!ast)
    return CompilerType();
  RustType *rtype = static_cast<RustType *>(type.GetOpaqueQualType());
  if (RustEnum *e = rtype->AsEnum()) {
    return e->FindEnumVariant(discriminant);
  }
  return CompilerType();
}

void
RustASTContext::FinishAggregateInitialization(const CompilerType &type) {
  if (!type)
    return;
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());
  if (!ast)
    return;
  RustType *rtype = static_cast<RustType *>(type.GetOpaqueQualType());
  if (RustAggregateBase *a = rtype->AsAggregate())
    a->FinishInitialization();
}

DWARFASTParser *RustASTContext::GetDWARFParser() {
  if (!m_dwarf_ast_parser_ap)
    m_dwarf_ast_parser_ap.reset(new DWARFASTParserRust(*this));
  return m_dwarf_ast_parser_ap.get();
}

UserExpression *RustASTContextForExpr::GetUserExpression(
    llvm::StringRef expr, llvm::StringRef prefix, lldb::LanguageType language,
    Expression::ResultType desired_type,
    const EvaluateExpressionOptions &options) {
  TargetSP target = m_target_wp.lock();
  if (target)
    return new RustUserExpression(*target, expr, prefix, language, desired_type,
                                  options);
  return nullptr;
}

ConstString RustASTContext::DeclGetName(void *opaque_decl) {
  RustDecl *dc = (RustDecl *) opaque_decl;
  return dc->Name();
}

ConstString RustASTContext::DeclGetMangledName(void *opaque_decl) {
  RustDecl *dc = (RustDecl *) opaque_decl;
  return dc->MangledName();
}

CompilerDeclContext RustASTContext::DeclGetDeclContext(void *opaque_decl) {
  RustDecl *dc = (RustDecl *) opaque_decl;
  return CompilerDeclContext(this, dc->Context());
}

ConstString RustASTContext::DeclContextGetName(void *opaque_decl_ctx) {
  RustDeclContext *dc = (RustDeclContext *) opaque_decl_ctx;
  return dc->Name();
}

ConstString RustASTContext::DeclContextGetScopeQualifiedName(void *opaque_decl_ctx) {
  RustDeclContext *dc = (RustDeclContext *) opaque_decl_ctx;
  return dc->QualifiedName();
}

bool RustASTContext::DeclContextIsStructUnionOrClass(void *opaque_decl_ctx) {
  // This is not actually correct -- for example an enum arm is nested
  // in its containing enum -- but as far as I can tell this result
  // doesn't matter for Rust.
  return false;
}

bool RustASTContext::DeclContextIsClassMethod(void *opaque_decl_ctx,
                                              lldb::LanguageType *language_ptr,
                                              bool *is_instance_method_ptr,
                                              ConstString *language_object_name_ptr) {
  return false;
}

std::vector<CompilerDecl>
RustASTContext::DeclContextFindDeclByName(void *opaque_decl_ctx, ConstString name,
                                          const bool ignore_imported_decls) {
  std::vector<CompilerDecl> result;
  SymbolFile *symbol_file = GetSymbolFile();
  if (symbol_file) {
    symbol_file->ParseDeclsForContext(CompilerDeclContext(this, opaque_decl_ctx));

    RustDeclContext *dc = (RustDeclContext *) opaque_decl_ctx;
    RustDeclBase *base = dc->FindByName(name);
    if (RustDecl *decl = base ? base->AsDecl() : nullptr) {
      result.push_back(CompilerDecl(this, decl));
    }
  }
  return result;
}

CompilerDeclContext RustASTContext::GetTranslationUnitDecl() {
  if (!m_tu_decl) {
    m_tu_decl.reset(new RustDeclContext(ConstString(""), nullptr));
  }
  return CompilerDeclContext(this, m_tu_decl.get());
}

CompilerDeclContext
RustASTContext::GetNamespaceDecl(CompilerDeclContext parent, const ConstString &name) {
  if (!parent)
    return CompilerDeclContext();
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(parent.GetTypeSystem());
  if (!ast)
    return CompilerDeclContext();

  RustDeclContext *dc = (RustDeclContext *) parent.GetOpaqueDeclContext();
  RustDeclBase *base = dc->FindByName(name);
  if (base) {
    if (RustDeclContext *ctx = base->AsDeclContext()) {
      return CompilerDeclContext(this, ctx);
    }
  }

  RustDeclContext *new_ns = new RustDeclContext(name, dc);
  dc->AddItem(std::unique_ptr<RustDeclBase>(new_ns));
  return CompilerDeclContext(this, new_ns);
}

CompilerDeclContext
RustASTContext::GetDeclContextDeclContext(CompilerDeclContext child) {
  if (!child)
    return CompilerDeclContext();
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(child.GetTypeSystem());
  if (!ast)
    return CompilerDeclContext();

  RustDeclContext *dc = (RustDeclContext *) child.GetOpaqueDeclContext();
  return CompilerDeclContext(this, dc->Context());
}

CompilerDecl RustASTContext::GetDecl(CompilerDeclContext parent, const ConstString &name,
                                     const ConstString &mangled) {
  if (!parent)
    return CompilerDecl();
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(parent.GetTypeSystem());
  if (!ast)
    return CompilerDecl();

  RustDeclContext *dc = (RustDeclContext *) parent.GetOpaqueDeclContext();
  RustDeclBase *base = dc->FindByName(name);
  if (base) {
    if (RustDecl *ctx = base->AsDecl()) {
      return CompilerDecl(this, ctx);
    }
  }

  RustDecl *new_ns = new RustDecl(name, mangled, dc);
  dc->AddItem(std::unique_ptr<RustDeclBase>(new_ns));
  return CompilerDecl(this, new_ns);
}

bool RustASTContext::GetCABITypeDeclaration(CompilerType type, const std::string &varname,
                                            RustASTContext::TypeNameMap *name_map,
                                            std::string *result) {
  if (!type)
    return false;
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());
  if (!ast)
    return false;
  RustType *rtype = static_cast<RustType *>(type.GetOpaqueQualType());
  *result = rtype->GetCABITypeDeclaration(name_map, varname);
  return true;
}
