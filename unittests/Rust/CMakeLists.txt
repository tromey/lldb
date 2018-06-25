add_lldb_unittest(RustTests
  RustLexTest.cpp RustParseTest.cpp

  LINK_LIBS
    lldbPluginExpressionParserRust
    lldbPluginLanguageRuntimeRust
    lldbCore
  )
