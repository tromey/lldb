//===-- RustLanguage.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
#include <string.h>
// C++ Includes
#include <functional>
#include <mutex>

// Other libraries and framework includes
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Threading.h"

// Project includes
#include "RustLanguage.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/DataFormatters/DataVisualization.h"
#include "lldb/DataFormatters/FormattersHelpers.h"
#include "lldb/Symbol/RustASTContext.h"
#include "lldb/Utility/ConstString.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;

void RustLanguage::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(), "Rust Language",
                                CreateInstance);
}

void RustLanguage::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

lldb_private::ConstString RustLanguage::GetPluginNameStatic() {
  static ConstString g_name("Rust");
  return g_name;
}

lldb_private::ConstString RustLanguage::GetPluginName() {
  return GetPluginNameStatic();
}

uint32_t RustLanguage::GetPluginVersion() { return 1; }

Language *RustLanguage::CreateInstance(lldb::LanguageType language) {
  if (language == eLanguageTypeRust)
    return new RustLanguage();
  return nullptr;
}

bool RustLanguage::IsSourceFile(llvm::StringRef file_path) const {
  return file_path.endswith(".rs");
}
