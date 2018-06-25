//===-- RustParse.h -------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_RustParse_h
#define liblldb_RustParse_h

#include "lldb/lldb-forward.h"
#include "lldb/lldb-private.h"
#include "RustLex.h"
#include "RustAST.h"

namespace lldb_private {

namespace rust {

class Parser {
public:

  Parser(lldb::TargetSP target, llvm::StringRef ref)
    : m_target(target),
      m_lexer(ref),
      // fixme this seems not good
      m_current(m_lexer.Next())
  {
  }

  RustExpressionUP ParseFully(Status &error) {
    RustExpressionUP result = Expr(error);
    if (CurrentToken().kind != THATSALLFOLKS) {
      error.SetErrorString("extra tokens at EOF");
      return RustExpressionUP();
    }
    return result;
  }

private:

  bool StartsTerm();
  template<char C, RustUnaryOperator OP> RustExpressionUP Unary(Status &error);
  bool ExprList(std::vector<RustExpressionUP> *exprs, Status &error);
  RustExpressionUP Parens(Status &error);
  RustExpressionUP Path(Status &error);
  RustExpressionUP Array(Status &error);
  RustExpressionUP Field(RustExpressionUP &&lhs, Status &error);
  RustExpressionUP Call(RustExpressionUP &&func, Status &error);
  RustExpressionUP Index(RustExpressionUP &&array, Status &error);
  RustExpressionUP Term(Status &error);
  RustExpressionUP Binary(Status &error);
  RustExpressionUP Sizeof(Status &error);
  RustExpressionUP Struct(RustTypeExpressionUP &&path, Status &error);
  RustExpressionUP Range(Status &error);

  RustExpressionUP Expr(Status &error) {
    return Range(error);
  }


  RustTypeExpressionUP Type(Status &error);
  RustTypeExpressionUP ArrayType(Status &error);
  RustTypeExpressionUP ReferenceType(Status &error);
  RustTypeExpressionUP PointerType(Status &error);
  bool TypeList(std::vector<RustTypeExpressionUP> *type_list, Status &error);
  bool ParenTypeList(std::vector<RustTypeExpressionUP> *type_list, Status &error);
  bool BracketTypeList(std::vector<RustTypeExpressionUP> *type_list, Status &error);
  RustTypeExpressionUP FunctionType(Status &error);
  RustTypeExpressionUP TupleType(Status &error);
  RustTypeExpressionUP TypePath(Status &error);

  Token &CurrentToken() { return m_current; }
  void Advance() { m_current = m_lexer.Next(); }

  // There's one case where we need to push-back, but we don't need
  // full generality so there's just this little hack.
  void ReplaceTokenKind(int k) { m_current.kind = k; }

  lldb::TargetSP m_target;
  Lexer m_lexer;
  Token m_current;
};

} // namespace rust
} // namespace lldb_private

#endif // liblldb_RustParse_h
