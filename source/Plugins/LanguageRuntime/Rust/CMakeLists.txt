add_lldb_library(lldbPluginLanguageRuntimeRust PLUGIN
  RustLanguageRuntime.cpp

  LINK_LIBS
    lldbCore
    lldbSymbol
    lldbTarget
  LINK_COMPONENTS
    Support
  )
