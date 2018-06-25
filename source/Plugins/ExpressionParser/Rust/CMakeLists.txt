add_lldb_library(lldbPluginExpressionParserRust PLUGIN
	RustUserExpression.cpp RustLex.cpp RustParse.cpp RustFunctionCaller.cpp

  LINK_LIBS
    lldbCore
    lldbExpression
    lldbSymbol
    lldbTarget
  LINK_COMPONENTS
    Support
  )
