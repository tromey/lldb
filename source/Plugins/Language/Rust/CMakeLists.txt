add_lldb_library(lldbPluginRustLanguage PLUGIN
  RustLanguage.cpp

  LINK_LIBS
    lldbCore
    lldbDataFormatters
    lldbSymbol
    lldbTarget
  LINK_COMPONENTS
    Support
)
