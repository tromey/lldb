//===-- DWARFASTParserRust.cpp ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "DWARFASTParserRust.h"

#include "DWARFASTParserRust.h"
#include "DWARFCompileUnit.h"
#include "DWARFDIE.h"
#include "DWARFDIECollection.h"
#include "DWARFDebugInfo.h"
#include "DWARFDeclContext.h"
#include "DWARFDefines.h"
#include "SymbolFileDWARF.h"
#include "SymbolFileDWARFDebugMap.h"
#include "UniqueDWARFASTType.h"

#include "clang/Basic/Specifiers.h"

#include "lldb/Core/Module.h"
#include "lldb/Core/Value.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/TypeList.h"

using namespace lldb;
using namespace lldb_private;

#define INVALID_ATTR dw_attr_t(-1)

// A way to iterate over DIE attrs.
class IterableDIEAttrs
{
public:
  IterableDIEAttrs(const DWARFDIE &die)
  {
    m_size = die.GetAttributes(m_attrs);
  }

  IterableDIEAttrs(const IterableDIEAttrs &) = delete;
  IterableDIEAttrs &operator=(const IterableDIEAttrs &) = delete;

  class iterator
  {
  public:

    iterator(const IterableDIEAttrs *die, size_t offset)
      : m_die(die)
      , m_offset(offset)
    {
    }

    iterator(const iterator &other)
      : m_die(other.m_die)
      , m_offset(other.m_offset)
    {
    }

    iterator &operator=(const iterator &other)
    {
      m_die = other.m_die;
      m_offset = other.m_offset;
      return *this;
    }

    iterator &operator++()
    {
      ++m_offset;
      return *this;
    }

    bool operator!=(const iterator &other) const
    {
      return m_die != other.m_die || m_offset != other.m_offset;
    }

    std::pair<dw_attr_t, DWARFFormValue> operator*() const
    {
      dw_attr_t attr = m_die->m_attrs.AttributeAtIndex(m_offset);
      DWARFFormValue value;
      if (!m_die->m_attrs.ExtractFormValueAtIndex(m_offset, value))
	attr = INVALID_ATTR;
      return std::make_pair(attr, value);
    }

  private:

    const IterableDIEAttrs *m_die;
    size_t m_offset;
  };

  iterator begin() const
  {
    return iterator(this, 0);
  }

  iterator end() const
  {
    return iterator(this, m_size);
  }

private:

  size_t m_size;
  DWARFAttributes m_attrs;
};

// A way to iterate over a DIE's direct children.
class IterableDIEChildren
{
public:
  IterableDIEChildren(const DWARFDIE &die)
    : m_die(die)
  {
  }

  IterableDIEChildren(const IterableDIEChildren &) = delete;
  IterableDIEChildren &operator=(const IterableDIEChildren &) = delete;

  class iterator
  {
  public:

    iterator(const DWARFDIE &die)
      : m_die(die)
    {
    }

    ~iterator()
    {
    }

    iterator(const iterator &other)
      : m_die(other.m_die)
    {
    }

    iterator &operator=(const iterator &other)
    {
      m_die = other.m_die;
      return *this;
    }

    iterator &operator++()
    {
      m_die = m_die.GetSibling();
      return *this;
    }

    bool operator!=(const iterator &other) const
    {
      return m_die != other.m_die;
    }

    DWARFDIE operator*() const
    {
      return m_die;
    }

  private:

    DWARFDIE m_die;
  };

  iterator begin() const
  {
    return iterator(m_die.GetFirstChild());
  }

  iterator end() const
  {
    return iterator(DWARFDIE(m_die.GetCU(), (DWARFDebugInfoEntry*) nullptr));
  }

private:

  DWARFDIE m_die;
};

ConstString DWARFASTParserRust::FullyQualify(const ConstString &name, const DWARFDIE &die) {
  SymbolFileDWARF *dwarf = die.GetDWARF();
  lldb::user_id_t id = die.GetID();
  CompilerDeclContext ctx = dwarf->GetDeclContextContainingUID(id);
  ConstString ctx_name = ctx.GetScopeQualifiedName();
  if (!ctx_name) {
    return name;
  }
  std::string qual_name = std::string(ctx_name.AsCString()) + "::" + name.AsCString();
  return ConstString(qual_name.c_str());
}

TypeSP DWARFASTParserRust::ParseSimpleType(lldb_private::Log *log, const DWARFDIE &die) {
  lldb::user_id_t encoding_uid = LLDB_INVALID_UID;
  const char *type_name_cstr = NULL;
  ConstString type_name_const_str;
  uint64_t byte_size = 0;
  uint64_t encoding = 0;

  for (auto &&value : IterableDIEAttrs(die)) {
    switch (value.first) {
    case DW_AT_name:
      type_name_cstr = value.second.AsCString();
      if (type_name_cstr)
	type_name_const_str.SetCString(type_name_cstr);
      break;
    case DW_AT_byte_size:
      byte_size = value.second.Unsigned();
      break;
    case DW_AT_encoding:
      encoding = value.second.Unsigned();
      break;
    case DW_AT_type:
      encoding_uid = value.second.Reference();
      break;
    }
  }

  SymbolFileDWARF *dwarf = die.GetDWARF();
  Type::ResolveState resolve_state = Type::eResolveStateUnresolved;
  CompilerType compiler_type;
  Type::EncodingDataType encoding_data_type = Type::eEncodingIsUID;
  switch (die.Tag()) {
  case DW_TAG_unspecified_type:
    resolve_state = Type::eResolveStateFull;
    compiler_type = m_ast.CreateVoidType();
    break;

  case DW_TAG_base_type:
    resolve_state = Type::eResolveStateFull;
    if (encoding == DW_ATE_boolean)
      compiler_type = m_ast.CreateBoolType(type_name_const_str);
    else if (encoding == DW_ATE_float)
      compiler_type = m_ast.CreateFloatType(type_name_const_str, byte_size);
    else if (encoding == DW_ATE_signed || encoding == DW_ATE_unsigned ||
             // DW_ATE_UCS seems to be less used (perhaps
             // Fortran-specific?) and since I'm not planning to have
             // rustc emit it, we ignore it here.
	     encoding == DW_ATE_unsigned_char || encoding == DW_ATE_UTF)
      compiler_type = m_ast.CreateIntegralType(type_name_const_str,
					       encoding == DW_ATE_signed,
					       byte_size,
                                               (encoding == DW_ATE_unsigned_char ||
                                                encoding == DW_ATE_UTF));
    else
      dwarf->GetObjectFile()->GetModule()->LogMessage(
          log, "DWARFASTParserRust::ParseSimpleType (die = 0x%8.8x) %s "
               "unrecognized encoding '%d')",
          die.GetOffset(), DW_TAG_value_to_name(die.Tag()), int(encoding));
    break;

    // Note that, currently, rustc does not emit DW_TAG_reference_type
    // - references are distinguished by name; and also we don't want
    // to treat Rust references as CompilerType references.
  case DW_TAG_typedef:
    type_name_const_str = FullyQualify(type_name_const_str, die);
    // Fall through.
  case DW_TAG_pointer_type:
  case DW_TAG_template_type_parameter: {
    Type *type = dwarf->ResolveTypeUID(encoding_uid);
    if (type) {
      CompilerType impl = type->GetForwardCompilerType();
      if (die.Tag() == DW_TAG_pointer_type) {
	int byte_size = die.GetCU()->GetAddressByteSize();
	m_ast.SetAddressByteSize(byte_size);
	compiler_type = m_ast.CreatePointerType(type_name_const_str, impl, byte_size);
	encoding_data_type = Type::eEncodingIsPointerUID;
      } else {
	compiler_type = m_ast.CreateTypedefType(type_name_const_str, impl);
	encoding_data_type = Type::eEncodingIsTypedefUID;
      }
    }
    break;
  }

  default:
    // Should have been filtered by the caller.
    assert(0);
  }

  return TypeSP(new Type(die.GetID(), dwarf, type_name_const_str,
			 byte_size, NULL, encoding_uid,
			 encoding_data_type, Declaration(), compiler_type,
			 resolve_state));
}

TypeSP DWARFASTParserRust::ParseArrayType(const DWARFDIE &die) {
  lldb::user_id_t type_die_offset = DW_INVALID_OFFSET;

  for (auto &&value : IterableDIEAttrs(die)) {
    switch (value.first) {
    case DW_AT_type:
      type_die_offset = value.second.Reference();
      break;
    }
  }

  SymbolFileDWARF *dwarf = die.GetDWARF();
  Type *element_type = dwarf->ResolveTypeUID(type_die_offset);
  if (!element_type)
    return TypeSP(nullptr);

  CompilerType compiler_type;
  uint64_t count = 0;

  for (auto &&child_die : IterableDIEChildren(die)) {
    if (child_die.Tag() == DW_TAG_subrange_type) {
      for (auto &&value : IterableDIEAttrs(child_die)) {
	if (value.first == DW_AT_count) {
	  count = value.second.Unsigned();
	  break;
	}
      }
      break;
    }
  }

  CompilerType array_element_type = element_type->GetForwardCompilerType();
  compiler_type = m_ast.CreateArrayType(array_element_type, count);

  ConstString type_name_const_str = compiler_type.GetTypeName();
  TypeSP type_sp(new Type(die.GetID(), dwarf, type_name_const_str,
			  element_type->GetByteSize(), NULL, type_die_offset,
			  Type::eEncodingIsUID, Declaration(), compiler_type,
			  Type::eResolveStateFull));
  type_sp->SetEncodingType(element_type);
  return type_sp;
}

TypeSP DWARFASTParserRust::ParseFunctionType(const DWARFDIE &die) {
  clang::StorageClass storage = clang::SC_None; //, Extern, Static, PrivateExtern
  const char *type_name_cstr = NULL;
  ConstString type_name_const_str;
  Declaration decl;

  CompilerType return_type;
  for (auto &&attr : IterableDIEAttrs(die)) {
    switch (attr.first) {
    case DW_AT_name:
      type_name_cstr = attr.second.AsCString();
      type_name_const_str.SetCString(type_name_cstr);
      break;

    case DW_AT_external:
      if (attr.second.Unsigned()) {
	if (storage == clang::SC_None)
	  storage = clang::SC_Extern;
	else
	  storage = clang::SC_PrivateExtern;
      }
      break;

    case DW_AT_type: {
      Type *type = die.ResolveTypeUID(DIERef(attr.second));
      if (type) {
	return_type = type->GetForwardCompilerType();
      }
      break;
    }
    }
  }

  if (!return_type) {
    return_type = m_ast.CreateVoidType();
  }

  std::vector<CompilerType> function_param_types;
  for (auto &&child_die : IterableDIEChildren(die)) {
    if (child_die.Tag() == DW_TAG_formal_parameter) {
      for (auto &&attr : IterableDIEAttrs(child_die)) {
	if (attr.first == DW_AT_type) {
	  Type *type = die.ResolveTypeUID(DIERef(attr.second));
	  if (type) {
	    function_param_types.push_back(type->GetForwardCompilerType());
	  }
	  break;
	}
      }
    }
  }

  CompilerType compiler_type = m_ast.CreateFunctionType(type_name_const_str, return_type,
							std::move(function_param_types));

  SymbolFileDWARF *dwarf = die.GetDWARF();
  TypeSP type_sp(new Type(die.GetID(), dwarf, type_name_const_str, 0, NULL,
			  LLDB_INVALID_UID, Type::eEncodingIsUID, &decl,
			  compiler_type, Type::eResolveStateFull));

  return type_sp;
}

static bool starts_with(const char *str, const char *prefix) {
  return strncmp(str, prefix, strlen(prefix)) == 0;
}

#define RUST_ENCODED_PREFIX "RUST$ENCODED$ENUM$"

std::vector<size_t> DWARFASTParserRust::ParseDiscriminantPath(const char **in_str) {
  std::vector<size_t> result;
  const char *str = *in_str;

  assert(starts_with(str, RUST_ENCODED_PREFIX));
  str += strlen(RUST_ENCODED_PREFIX);

  // We're going to push a synthetic unit struct field as the enum
  // type's first member, so the resulting discriminant path always
  // starts with 1.
  result.push_back(1);

  while (*str >= '0' && *str <= '9') {
    char *next;
    unsigned long value = strtoul(str, &next, 10);
    result.push_back(value);
    str = next;
    if (*str != '$') {
      // Report an error?
      *in_str = nullptr;
      result.clear();
      return result;
    }
    ++str;
  }

  // At this point, STR points to the name of the elided member type.
  *in_str = str;
  return result;
}

void DWARFASTParserRust::FindDiscriminantLocation(CompilerType type,
						  std::vector<size_t> &&path,
						  uint64_t &offset,
						  uint64_t &byte_size) {
  offset = 0;

  for (size_t index : path) {
    std::string ignore_name;
    uint32_t bitsize_ignore, bitoffset_ignore;
    bool isbase_ignore, isderef_ignore;
    uint64_t lang_flags_ignore;

    uint32_t this_size;
    int32_t this_offset;

    type = m_ast.GetChildCompilerTypeAtIndex(type.GetOpaqueQualType(), nullptr, index,
					     false, false, true,
					     ignore_name,
					     this_size, this_offset,
					     bitsize_ignore, bitoffset_ignore,
					     isbase_ignore, isderef_ignore,
					     nullptr, lang_flags_ignore);
    offset += this_offset;
    // The last time this is done, it will hold the size of the final
    // field, which is what we want.
    byte_size = this_size;
  }
}

bool DWARFASTParserRust::IsPossibleEnumVariant(const DWARFDIE &die) {
  if (die.Tag() != DW_TAG_structure_type) {
    // Only structures can be enum variants.
    return false;
  }

  for (auto &&child_die : IterableDIEChildren(die)) {
    if (child_die.Tag() == DW_TAG_member) {
      for (auto &&attr : IterableDIEAttrs(child_die)) {
	if (attr.first == DW_AT_name) {
	  return strcmp(attr.second.AsCString(), "RUST$ENUM$DISR") == 0;
        }
      }
      // No name, so whatever it is, it isn't an enum variant.
      return false;
    }
  }

  // We didn't see a member, and an empty structure might well be an
  // enum variant.
  return true;
}

std::vector<DWARFASTParserRust::Field>
DWARFASTParserRust::ParseFields(const DWARFDIE &die, std::vector<size_t> &discriminant_path,
				bool &is_tuple,
				uint64_t &discr_offset, uint64_t &discr_byte_size,
				bool &saw_discr) {
  SymbolFileDWARF *dwarf = die.GetDWARF();

  // We construct a list of fields and then apply them later so that
  // we can analyze the fields to see what sort of structure this
  // really is.
  std::vector<Field> fields;
  unsigned field_index = 0;
  bool numeric_names = true;

  // We might have recursed in here with a variant part.  If so, we
  // want to handle the discriminant and variants specially.
  bool is_variant = die.Tag() == DW_TAG_variant_part;
  DWARFDIE discriminant_die;
  if (is_variant) {
    discriminant_die = die.GetReferencedDIE(DW_AT_discr);
  }

  // For old-style ("RUST$ENUM$DISR"-using) enums that don't have the
  // NonZero optimization applied, variants are listed in order of
  // discriminant.  We track that value here.
  uint64_t naive_discriminant = 0;

  bool could_be_enum = die.Tag() == DW_TAG_union_type;
  bool encoded_enum = false;

  ModuleSP module_sp = die.GetModule();
  for (auto &&child_die : IterableDIEChildren(die)) {
    Field new_field;

    // If this isn't correct for this particular enum, that's ok,
    // because the correct value will be computed below.
    new_field.discriminant = naive_discriminant++;

    if (is_variant && child_die.Tag() == DW_TAG_variant) {
      // Find the discriminant, if it exists.
      for (auto &&attr : IterableDIEAttrs(child_die)) {
	if (attr.first == DW_AT_discr_value) {
	  new_field.discriminant = attr.second.Unsigned();
	  break;
	}
      }

      // Use the child that is a member.
      bool found = false;
      for (auto &&variant_child_die : IterableDIEChildren(child_die)) {
	if (variant_child_die.Tag() == DW_TAG_member) {
	  found = true;
	  child_die = variant_child_die;
	  break;
	}
      }
      if (!found) {
	// Just ignore this variant.
	continue;
      }

      // Fall through and process the variant's child as if it were a
      // child of the structure.
    }

    if (child_die.Tag() == DW_TAG_member) {
      for (auto &&attr : IterableDIEAttrs(child_die)) {
	switch (attr.first) {
	case DW_AT_name:
	  new_field.name = attr.second.AsCString();
	  if (fields.size() == 0) {
	    if (strcmp(new_field.name, "RUST$ENUM$DISR") == 0)
	      new_field.is_discriminant = true;
	    else if (starts_with(new_field.name, RUST_ENCODED_PREFIX)) {
	      // The "non-zero" optimization has been applied.
	      // In this case, we'll see a single field like:
	      //   RUST$ENCODED$ENUM$n0$n1...$Name
	      // Here n0, n1, ... are integers that describe the path
	      // to the discriminant.  When the discriminant (and
	      // integer) is 0, the enum has the value Name, a
	      // unit-like struct.  However when it is non-zero, the
	      // enum has the value of this field's type.

	      // Here we're going to push an initial field for the
	      // unit-like struct.  Note that the constructor sets the
	      // discriminant to the correct value -- zero.
	      Field unit_field;
	      unit_field.name = new_field.name;
	      discriminant_path = ParseDiscriminantPath(&unit_field.name);
	      unit_field.is_elided = true;
	      fields.push_back(unit_field);

	      // The actual field is the default variant.
	      new_field.is_default = true;
              new_field.name = nullptr;
              encoded_enum = true;
	    }
	  }
	  break;
	case DW_AT_type:
	  new_field.type = attr.second;
          if (could_be_enum && !encoded_enum) {
            could_be_enum = IsPossibleEnumVariant(dwarf->GetDIE(DIERef(new_field.type)));
          }
	  break;
	case DW_AT_data_member_location:
	  if (attr.second.BlockData()) {
	    Value initialValue(0);
	    Value memberOffset(0);
	    const DWARFDataExtractor &debug_info_data =
	      child_die.GetDWARF()->get_debug_info_data();
	    uint32_t block_length = attr.second.Unsigned();
	    uint32_t block_offset = attr.second.BlockData() - debug_info_data.GetDataStart();
	    if (DWARFExpression::Evaluate(
					  NULL, // ExecutionContext *
					  NULL, // RegisterContext *
					  module_sp, debug_info_data, die.GetCU(), block_offset,
					  block_length, eRegisterKindDWARF, &initialValue, NULL,
					  memberOffset, NULL)) {
	      new_field.byte_offset = memberOffset.ResolveValue(NULL).UInt();
	    }
	  } else {
	    new_field.byte_offset = attr.second.Unsigned();
	  }
	  break;
	}
      }
    }

    if (child_die == discriminant_die) {
      // This field is the discriminant, so don't push it, but instead
      // record this for the caller.
      saw_discr = true;
      discr_offset = new_field.byte_offset;

      Type *type = die.ResolveTypeUID(DIERef(new_field.type));
      if (type) {
	lldb_private::CompilerType ctype = type->GetFullCompilerType();
	discr_byte_size = m_ast.GetBitSize(ctype.GetOpaqueQualType(), nullptr) / 8;
      }
    } else if (child_die.Tag() == DW_TAG_variant_part) {
      // New-style enum representation -- nothing useful is in the
      // enclosing struct, so we can just recurse here.
      return ParseFields(child_die, discriminant_path, is_tuple,
			 discr_offset, discr_byte_size, saw_discr);
    } else {
      if (new_field.is_discriminant) {
	// Don't check this field name, and don't increment field_index.
	// When we see a tuple with fields like
	//   RUST$ENUM$DISR
	//   __0
	//   __1
	//   etc
	// ... it means the tuple is a member type of an enum.
      } else if (numeric_names) {
	char buf[32];
	snprintf (buf, sizeof (buf), "__%u", field_index);
	if (!new_field.name || strcmp(new_field.name, buf) != 0)
	  numeric_names = false;
	++field_index;
      }

      fields.push_back(new_field);
    }
  }

  if (!numeric_names) {
    // If the field name checking failed, maybe we don't have a tuple
    // after all, somehow.
    is_tuple = false;
  } else if (!is_tuple) {
    // If we saw numeric names in sequence, we have a tuple struct;
    // but if there were no fields, then we can't tell and so we
    // arbitrarily choose an empty struct.
    is_tuple = field_index > 0;
  }

  // If we saw a Rust enum, correctly arrange the scope of the various
  // sub-types.  This is needed to work around the way that the Rust
  // compiler emits the types: it emits each enum variant's type as a
  // sibling of the enum type, whereas logically it ought to be a
  // child.
  if (could_be_enum) {
    for (auto &&field : fields) {
      m_reparent_map[dwarf->GetDIE(DIERef(field.type)).GetDIE()] = die;
    }
  }

  return fields;
}

TypeSP DWARFASTParserRust::ParseStructureType(const DWARFDIE &die) {
  const bool is_union = die.Tag() == DW_TAG_union_type;

  bool byte_size_valid = false;
  uint64_t byte_size = 0;
  const char *type_name_cstr = NULL;
  ConstString type_name_const_str;
  SymbolFileDWARF *dwarf = die.GetDWARF();
  Declaration decl;

  for (auto &&attr : IterableDIEAttrs(die)) {
    switch (attr.first) {
    case DW_AT_name:
      type_name_cstr = attr.second.AsCString();
      type_name_const_str.SetCString(type_name_cstr);
      break;

    case DW_AT_byte_size:
      byte_size = attr.second.Unsigned();
      byte_size_valid = true;
      break;
    }
  }

  UniqueDWARFASTType ast_entry;
  TypeSP type_sp;

  // Only try and unique the type if it has a name.
  if (type_name_const_str &&
      dwarf->GetUniqueDWARFASTTypeMap().Find(type_name_const_str, die, &decl,
					     byte_size_valid ? byte_size : -1, ast_entry)) {
    // We have already parsed this type.
    type_sp = ast_entry.m_type_sp;
    if (type_sp) {
      dwarf->m_die_to_type[die.GetDIE()] = type_sp.get();
      return type_sp;
    }
  }

  // Currently, rustc emits tuples with a name starting with "("; but
  // there's no way to distinguish a zero-length struct from a
  // zero-length tuple struct.  This decision might be changed by
  // ParseFields.
  bool is_tuple = type_name_cstr && type_name_cstr[0] == '(';
  // We might see a tuple struct, and we want to differentiate the two
  // when qualifying names.
  bool is_anon_tuple = is_tuple;
  bool saw_discr = false;
  uint64_t discr_offset, discr_byte_size;
  std::vector<size_t> discriminant_path;
  std::vector<Field> fields = ParseFields(die, discriminant_path, is_tuple,
					  discr_offset, discr_byte_size, saw_discr);

  // This is true if this is a union, there are multiple fields and
  // each field's type has a discriminant.
  bool all_have_discriminants = is_union && fields.size() > 0;
  // This is true if the current type has a discriminant.
  // all_have_discriminants records whether the outer type is a Rust
  // enum; this records whether the current type is one variant type
  // of the enum.
  bool has_discriminant = fields.size() > 0 && fields[0].is_discriminant;

  // See the comment by m_discriminant to understand this.
  DIERef save_discr = m_discriminant;
  if (has_discriminant)
    m_discriminant = DIERef(fields[0].type);

  // Have to resolve the field types before creating the outer type,
  // so that we can see whether or not this is an enum.
  for (auto &&field : fields) {
    if (field.is_elided) {
      // A unit-like struct with the given name.  The byte size
      // probably doesn't matter.
      ConstString name = FullyQualify(type_name_const_str, die);
      name = ConstString((std::string(name.AsCString()) + "::" + field.name).c_str());
      field.compiler_type = m_ast.CreateStructType(name, 1, false);
    } else {
      Type *type = die.ResolveTypeUID(DIERef(field.type));
      if (type) {
	field.compiler_type = type->GetFullCompilerType();
	if (all_have_discriminants)
	  all_have_discriminants = m_ast.TypeHasDiscriminant(field.compiler_type);
      }
    }

    // Fix up the field's name by taking it from the type if necessary.
    if (field.name == nullptr) {
      field.name = field.compiler_type.GetTypeName().AsCString();
    }
  }

  m_discriminant = save_discr;

  bool compiler_type_was_created = false;
  CompilerType compiler_type(&m_ast,
			     dwarf->m_forward_decl_die_to_clang_type.lookup(die.GetDIE()));
  if (!compiler_type) {
    compiler_type_was_created = true;

    if (!is_anon_tuple) {
      type_name_const_str = FullyQualify(type_name_const_str, die);
    }

    if (saw_discr) {
      compiler_type = m_ast.CreateEnumType(type_name_const_str, byte_size,
					   discr_offset, discr_byte_size);
    } else if (all_have_discriminants) {
      // In this case, the discriminant is easily computed as the 0th
      // field of the 0th field.
      discriminant_path = std::vector<size_t> { 0 };

      FindDiscriminantLocation(fields[0].compiler_type, std::move(discriminant_path),
			       discr_offset, discr_byte_size);

      compiler_type = m_ast.CreateEnumType(type_name_const_str, byte_size,
					   discr_offset, discr_byte_size);
    } else if (!discriminant_path.empty()) {
      CompilerType start_type = fields[discriminant_path[0]].compiler_type;
      discriminant_path.erase(discriminant_path.begin());

      FindDiscriminantLocation(start_type, std::move(discriminant_path),
			       discr_offset, discr_byte_size);

      compiler_type = m_ast.CreateEnumType(type_name_const_str, byte_size,
					   discr_offset, discr_byte_size);
    } else if (is_union)
      compiler_type = m_ast.CreateUnionType(type_name_const_str, byte_size);
    else if (is_tuple)
      compiler_type = m_ast.CreateTupleType(type_name_const_str, byte_size, has_discriminant);
    else
      compiler_type = m_ast.CreateStructType(type_name_const_str, byte_size, has_discriminant);
  }

  type_sp.reset(new Type(die.GetID(), dwarf, type_name_const_str,
			 byte_size, NULL, LLDB_INVALID_UID,
			 Type::eEncodingIsUID, &decl, compiler_type,
			 Type::eResolveStateForward));

  // Now add the fields.
  for (auto &&field : fields) {
    if (field.compiler_type) {
      ConstString name(is_tuple ? "" : field.name);
      m_ast.AddFieldToStruct(compiler_type, name, field.compiler_type, field.byte_offset,
			     field.is_default, field.discriminant);
    }
  }

  m_ast.FinishAggregateInitialization(compiler_type);

  // Add our type to the unique type map so we don't
  // end up creating many copies of the same type over
  // and over in the ASTContext for our module
  ast_entry.m_type_sp = type_sp;
  ast_entry.m_die = die;
  ast_entry.m_declaration = decl;
  ast_entry.m_byte_size = byte_size;
  dwarf->GetUniqueDWARFASTTypeMap().Insert(type_name_const_str, ast_entry);

  if (compiler_type_was_created) {
    // Leave this as a forward declaration until we need
    // to know the details of the type. lldb_private::Type
    // will automatically call the SymbolFile virtual function
    // "SymbolFileDWARF::CompleteType(Type *)"
    // When the definition needs to be defined.
    dwarf->m_forward_decl_die_to_clang_type[die.GetDIE()] =
      compiler_type.GetOpaqueQualType();
    dwarf->m_forward_decl_clang_type_to_die[compiler_type.GetOpaqueQualType()] =
      die.GetDIERef();
  }

  return type_sp;
}

TypeSP DWARFASTParserRust::ParseCLikeEnum(lldb_private::Log *log, const DWARFDIE &die) {
  const char *type_name_cstr = NULL;
  ConstString type_name_const_str;
  SymbolFileDWARF *dwarf = die.GetDWARF();
  CompilerType underlying_type;

  for (auto &&attr : IterableDIEAttrs(die)) {
    switch (attr.first) {
    case DW_AT_name:
      type_name_cstr = attr.second.AsCString();
      type_name_const_str.SetCString(type_name_cstr);
      break;

    case DW_AT_type:
      if (Type *type = die.ResolveTypeUID(DIERef(attr.second))) {
	underlying_type = type->GetFullCompilerType();
      }
      break;
    }
  }

  // See the comment by m_discriminant to understand this; but this
  // allows registering two types of the same name when reading a Rust
  // enum.
  if (die.GetDIERef() == m_discriminant) {
    type_name_const_str.Clear();
  } else {
    type_name_const_str = FullyQualify(type_name_const_str, die);
  }

  std::map<uint64_t, std::string> values;
  for (auto &&child_die : IterableDIEChildren(die)) {
    if (child_die.Tag() != DW_TAG_enumerator) {
      continue;
    }

    bool saw_value = false;
    uint64_t value;
    std::string name;
    for (auto &&attr : IterableDIEAttrs(child_die)) {
      switch (attr.first) {
      case DW_AT_name:
	name = attr.second.AsCString();
	break;
      case DW_AT_const_value:
	saw_value = true;
	value = attr.second.Unsigned();
	break;
      }

      if (saw_value && !name.empty()) {
	values[value] = name;
      } else {
	dwarf->GetObjectFile()->GetModule()->LogMessage(
          log, "DWARFASTParserRust::ParseCLikeEnum (die = 0x%8.8x) %s "
               "is invalid)",
          child_die.GetOffset(), DW_TAG_value_to_name(die.Tag()));
      }
    }
  }

  Declaration decl;
  CompilerType compiler_type = m_ast.CreateCLikeEnumType(type_name_const_str,
							 underlying_type,
							 std::move(values));
  TypeSP type_sp(new Type(die.GetID(), dwarf, type_name_const_str, 0, NULL,
			  LLDB_INVALID_UID, Type::eEncodingIsUID, &decl,
			  compiler_type, Type::eResolveStateFull));

  return type_sp;
}

TypeSP DWARFASTParserRust::ParseTypeFromDWARF(
    const lldb_private::SymbolContext &sc, const DWARFDIE &die,
    lldb_private::Log *log, bool *type_is_new_ptr) {
  TypeSP type_sp;

  if (type_is_new_ptr)
    *type_is_new_ptr = false;

  if (die) {
    SymbolFileDWARF *dwarf = die.GetDWARF();
    if (log) {
      dwarf->GetObjectFile()->GetModule()->LogMessage(
          log, "DWARFASTParserRust::ParseTypeFromDWARF (die = 0x%8.8x) %s name = "
               "'%s')",
          die.GetOffset(), DW_TAG_value_to_name(die.Tag()), die.GetName());
    }

    Type *type_ptr = dwarf->m_die_to_type.lookup(die.GetDIE());
    TypeList *type_list = dwarf->GetTypeList();
    if (type_ptr == NULL) {
      if (type_is_new_ptr)
        *type_is_new_ptr = true;

      const dw_tag_t tag = die.Tag();

      // Set a bit that lets us know that we are currently parsing this
      dwarf->m_die_to_type[die.GetDIE()] = DIE_IS_BEING_PARSED;

      switch (tag) {
      case DW_TAG_base_type:
      case DW_TAG_pointer_type:
      case DW_TAG_typedef:
      case DW_TAG_template_type_parameter:
      case DW_TAG_unspecified_type:
	type_sp = ParseSimpleType(log, die);
	break;

      case DW_TAG_union_type:
      case DW_TAG_structure_type:
	type_sp = ParseStructureType(die);
	break;

      case DW_TAG_subprogram:
      case DW_TAG_subroutine_type:
	type_sp = ParseFunctionType(die);
	break;

      case DW_TAG_array_type:
	type_sp = ParseArrayType(die);
	break;

      case DW_TAG_enumeration_type:
	type_sp = ParseCLikeEnum(log, die);
	break;

      default:
        dwarf->GetObjectFile()->GetModule()->ReportError(
            "{0x%8.8x}: unhandled type tag 0x%4.4x (%s), "
            "please file a bug and attach the file at the "
            "start of this error message",
            die.GetOffset(), tag, DW_TAG_value_to_name(tag));
        break;
      }

      if (type_sp.get()) {
        DWARFDIE sc_parent_die =
            SymbolFileDWARF::GetParentSymbolContextDIE(die);
        dw_tag_t sc_parent_tag = sc_parent_die.Tag();

        SymbolContextScope *symbol_context_scope = NULL;
        if (sc_parent_tag == DW_TAG_compile_unit) {
          symbol_context_scope = sc.comp_unit;
        } else if (sc.function != NULL && sc_parent_die) {
          symbol_context_scope =
              sc.function->GetBlock(true).FindBlockByID(sc_parent_die.GetID());
          if (symbol_context_scope == NULL)
            symbol_context_scope = sc.function;
        }

        if (symbol_context_scope != NULL) {
          type_sp->SetSymbolContextScope(symbol_context_scope);
        }

        // We are ready to put this type into the uniqued list up at the module
        // level
        type_list->Insert(type_sp);
      }
      dwarf->m_die_to_type[die.GetDIE()] = type_sp.get();
    } else if (type_ptr != DIE_IS_BEING_PARSED) {
      type_sp = type_ptr->shared_from_this();
    }
  }
  return type_sp;
}

bool DWARFASTParserRust::CompleteTypeFromDWARF(const DWARFDIE &die,
					       lldb_private::Type *type,
					       CompilerType &compiler_type) {
  // We don't currently use type completion for Rust.
  return bool(die);
}

Function *DWARFASTParserRust::ParseFunctionFromDWARF(const SymbolContext &sc,
						     const DWARFDIE &die) {
  DWARFRangeList func_ranges;
  const char *name = NULL;
  const char *mangled = NULL;
  int decl_file = 0;
  int decl_line = 0;
  int decl_column = 0;
  int call_file = 0;
  int call_line = 0;
  int call_column = 0;
  DWARFExpression frame_base(die.GetCU());

  assert(die.Tag() == DW_TAG_subprogram);

  if (die.GetDIENamesAndRanges(name, mangled, func_ranges, decl_file, decl_line,
                               decl_column, call_file, call_line, call_column,
                               &frame_base)) {
    // Union of all ranges in the function DIE (if the function is
    // discontiguous)
    AddressRange func_range;
    lldb::addr_t lowest_func_addr = func_ranges.GetMinRangeBase(0);
    lldb::addr_t highest_func_addr = func_ranges.GetMaxRangeEnd(0);
    if (lowest_func_addr != LLDB_INVALID_ADDRESS &&
        lowest_func_addr <= highest_func_addr) {
      ModuleSP module_sp(die.GetModule());
      func_range.GetBaseAddress().ResolveAddressUsingFileSections(
          lowest_func_addr, module_sp->GetSectionList());
      if (func_range.GetBaseAddress().IsValid())
        func_range.SetByteSize(highest_func_addr - lowest_func_addr);
    }

    if (func_range.GetBaseAddress().IsValid()) {
      Mangled func_name;
      func_name.SetValue(ConstString(name), false);

      FunctionSP func_sp;
      std::unique_ptr<Declaration> decl_ap;
      if (decl_file != 0 || decl_line != 0 || decl_column != 0)
        decl_ap.reset(new Declaration(
            sc.comp_unit->GetSupportFiles().GetFileSpecAtIndex(decl_file),
            decl_line, decl_column));

      SymbolFileDWARF *dwarf = die.GetDWARF();
      // Supply the type _only_ if it has already been parsed
      Type *func_type = dwarf->m_die_to_type.lookup(die.GetDIE());

      assert(func_type == NULL || func_type != DIE_IS_BEING_PARSED);

      if (dwarf->FixupAddress(func_range.GetBaseAddress())) {
        const user_id_t func_user_id = die.GetID();
        func_sp.reset(new Function(sc.comp_unit,
                                   func_user_id, // UserID is the DIE offset
                                   func_user_id, func_name, func_type,
                                   func_range)); // first address range

        if (func_sp.get() != NULL) {
          if (frame_base.IsValid())
            func_sp->GetFrameBaseExpression() = frame_base;
          sc.comp_unit->AddFunction(func_sp);
          return func_sp.get();
        }
      }
    }
  }
  return NULL;
}

lldb_private::CompilerDeclContext
DWARFASTParserRust::GetDeclContextForUIDFromDWARF(const DWARFDIE &die) {
  auto iter = m_decl_contexts.find(die.GetDIE());
  if (iter != m_decl_contexts.end()) {
    return iter->second;
  }

  CompilerDeclContext result;
  switch (die.Tag()) {
  case DW_TAG_compile_unit:
    result = m_ast.GetTranslationUnitDecl();
    break;

  case DW_TAG_union_type:
  case DW_TAG_structure_type:
  case DW_TAG_namespace: {
    const char *name = die.GetName();
    if (name) {
      CompilerDeclContext parent = GetDeclContextContainingUIDFromDWARF(die);
      result = m_ast.GetNamespaceDecl(parent, ConstString(name));
    }
    break;
  }

  case DW_TAG_lexical_block:
  case DW_TAG_subprogram:
    result = GetDeclContextContainingUIDFromDWARF(die);
    break;

  default:
    break;
  }

  if (result) {
    m_decl_contexts[die.GetDIE()] = result;
    m_decl_contexts_to_die.emplace(result, die);
  }

  return result;
}

lldb_private::CompilerDeclContext
DWARFASTParserRust::GetDeclContextContainingUIDFromDWARF(const DWARFDIE &die) {
  DWARFDIE decl_ctx_die;

  DWARFDebugInfoEntry *ptr = die.GetDIE();
  auto iter = m_reparent_map.find(ptr);
  if (iter != m_reparent_map.end()) {
    decl_ctx_die = iter->second;
  } else {
    SymbolFileDWARF *dwarf = die.GetDWARF();
    decl_ctx_die = dwarf->GetDeclContextDIEContainingDIE(die);
  }
  return GetDeclContextForUIDFromDWARF(decl_ctx_die);
}

lldb_private::CompilerDecl
DWARFASTParserRust::GetDeclForUIDFromDWARF(const DWARFDIE &die) {
  auto iter = m_decls.find(die.GetDIE());
  if (iter != m_decls.end()) {
    return iter->second;
  }

  CompilerDecl result;
  if (die.Tag() == DW_TAG_variable || die.Tag() == DW_TAG_constant) {
    const char *name = die.GetName();
    if (name) {
      const char *mangled = die.GetMangledName();
      CompilerDeclContext parent = GetDeclContextContainingUIDFromDWARF(die);
      result = m_ast.GetDecl(parent, ConstString(name), ConstString(mangled));

      if (result) {
        m_decls[die.GetDIE()] = result;
      }
    }
  }

  return result;
}

std::vector<DWARFDIE>
DWARFASTParserRust::GetDIEForDeclContext(lldb_private::CompilerDeclContext decl_context) {
  std::vector<DWARFDIE> result;
  for (auto it = m_decl_contexts_to_die.find(decl_context);
       it != m_decl_contexts_to_die.end();
       ++it)
    result.push_back(it->second);
  return result;
}
