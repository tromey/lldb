//===-- RustLexTest.cpp ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include <cstdarg>
#include "Plugins/ExpressionParser/Rust/RustLex.h"
#include "lldb/Utility/Status.h"

using namespace lldb_private;
using namespace lldb_private::rust;

template<std::size_t N>
static void
TestLex(const char *input, const std::array<Token, N> &expected) {
  Lexer lexer(input);

  for (std::size_t i = 0; i < N; ++i) {
    EXPECT_EQ(lexer.Next(), expected[i]) << "tokens differ at index " << i
                                         << " for \"" << input << "\"";
  }
  EXPECT_EQ(lexer.Next(), Token(THATSALLFOLKS)) << "EOF expected for input \""
                                                << input << "\"";
}

static void
TestSingle(const char *input, const Token &token) {
  std::array<Token, 1> tokens { token };
  TestLex(input, tokens);
}

TEST(RustLexTest, Keywords) {
  TestSingle("as", Token(AS));
  TestSingle("true", Token(TRUE));
  TestSingle("false", Token(FALSE));
  TestSingle("super", Token(SUPER));
  TestSingle("self", Token(SELF));
  TestSingle("mut", Token(MUT));
  TestSingle("const", Token(CONST));
  TestSingle("fn", Token(FN));
  TestSingle("sizeof", Token(SIZEOF));
  TestSingle("..", Token(DOTDOT));
  TestSingle("..=", Token(DOTDOTEQ));
  TestSingle("||", Token(OROR));
  TestSingle("|=", Token(OR_EQ));
  TestSingle("&&", Token(ANDAND));
  TestSingle("&=", Token(AND_EQ));
  TestSingle("^=", Token(XOR_EQ));
  TestSingle("==", Token(EQEQ));
  TestSingle("!=", Token(NOTEQ));
  TestSingle("<=", Token(LTEQ));
  TestSingle(">=", Token(GTEQ));
  TestSingle("<<", Token(LSH));
  TestSingle(">>", Token(RSH));
  TestSingle("+=", Token(PLUS_EQ));
  TestSingle("-=", Token(MINUS_EQ));
  TestSingle("*=", Token(STAR_EQ));
  TestSingle("/=", Token(SLASH_EQ));
  TestSingle("%=", Token(PERCENT_EQ));
  TestSingle("<<=", Token(LSH_EQ));
  TestSingle(">>=", Token(RSH_EQ));
  TestSingle("::", Token(COLONCOLON));
  TestSingle("->", Token(ARROW));
  TestSingle(".", Token('.'));
  TestSingle("|", Token('|'));
  TestSingle("&", Token('&'));
  TestSingle("=", Token('='));
  TestSingle("!", Token('!'));
  TestSingle("<", Token('<'));
  TestSingle(">", Token('>'));
  TestSingle("+", Token('+'));
  TestSingle("-", Token('-'));
  TestSingle("*", Token('*'));
  TestSingle("/", Token('/'));
  TestSingle("%", Token('%'));
  TestSingle(":", Token(':'));
  TestSingle("[", Token('['));
  TestSingle("]", Token(']'));
  TestSingle("(", Token('('));
  TestSingle(")", Token(')'));
  TestSingle("{", Token('{'));
  TestSingle("}", Token('}'));
}

TEST(RustLexTest, Identifiers) {
  TestSingle("one", Token(IDENTIFIER, "one"));
  TestSingle("o_e", Token(IDENTIFIER, "o_e"));
  TestSingle("_E001__Z", Token(IDENTIFIER, "_E001__Z"));
  TestSingle("b", Token(IDENTIFIER, "b"));
  TestSingle("br", Token(IDENTIFIER, "br"));
  TestSingle("brrrr", Token(IDENTIFIER, "brrrr"));
}

TEST(RustLexTest, Integers) {
  TestSingle("1", Token(INTEGER, uint64_t(1)));
  TestSingle("1usize", Token(INTEGER, uint64_t(1), "usize"));
  TestSingle("2_33", Token(INTEGER, uint64_t(233)));
  TestSingle("2_33i64", Token(INTEGER, uint64_t(233), "i64"));
  TestSingle("0x_ff", Token(INTEGER, uint64_t(255)));
  TestSingle("0xFf", Token(INTEGER, uint64_t(255)));
  TestSingle("0b_1111_1111", Token(INTEGER, uint64_t(255)));
  TestSingle("0o11_u64", Token(INTEGER, uint64_t(9), "u64"));
}

TEST(RustLexTest, Floats) {
  TestSingle("1.0", Token(FLOAT, 1.0));
  TestSingle("1.5f32", Token(FLOAT, 1.5, "f32"));
  TestSingle("1.5E+02", Token(FLOAT, 150.0));
  TestSingle("1.5e+03", Token(FLOAT, 1500.0));
  TestSingle("1e+03f64", Token(FLOAT, 1000.0, "f64"));
}

TEST(RustLexTest, ByteLiteral) {
  TestSingle("b'b'", Token(BYTE, uint64_t('b')));
  TestSingle("b'\\''", Token(BYTE, uint64_t('\'')));
  TestSingle("b'\\x9A'", Token(BYTE, uint64_t(154)));
  TestSingle("b'\\x9A", Token(INVALID));
  TestSingle("b'", Token(INVALID));
  // TestSingle("b'\\u{9A}'", Token(INVALID));
}

TEST(RustLexTest, Characters) {
  TestSingle("'b'", Token(CHAR, uint64_t('b')));
  TestSingle("'\\''", Token(CHAR, uint64_t('\'')));
  TestSingle("'\\x9A'", Token(CHAR, uint64_t(154)));
  TestSingle("'\\x9A'", Token(CHAR, uint64_t(154)));
  TestSingle("'\\x9A", Token(INVALID));
  TestSingle("'\\u{9A}'", Token(CHAR, uint64_t(154)));
  TestSingle("'\\u{00009A}'", Token(CHAR, uint64_t(154)));
  TestSingle("'\\u{0000fff", Token(INVALID));
  TestSingle("'\\u{0000ff}", Token(INVALID));
  TestSingle("'", Token(INVALID));
}

TEST(RustLexTest, Strings) {
  TestSingle("\"hi\"", Token(STRING, "hi"));
  TestSingle("\"\\u{68}\\u{000069}\"", Token(STRING, "hi"));
  TestSingle("\"\\x68\\x69\"", Token(STRING, "hi"));
}

TEST(RustLexTest, RawStrings) {
  TestSingle("r\"hi\"", Token(STRING, "hi"));
  TestSingle("r########\"hi\"########", Token(STRING, "hi"));
  TestSingle("r########\"h\"#######i\"########", Token(STRING, "h\"#######i"));
  TestSingle("r########\"\\'\"########", Token(STRING, "\\'"));
}

TEST(RustLexTest, RawByteStrings) {
  TestSingle("br\"hi\"", Token(BYTESTRING, "hi"));
  TestSingle("br########\"hi\"########", Token(BYTESTRING, "hi"));
  TestSingle("br########\"h\"#######i\"########", Token(BYTESTRING, "h\"#######i"));
  TestSingle("br########\"\\'\"########", Token(BYTESTRING, "\\'"));
}

TEST(RustLexTest, Streams) {
  TestLex("57 as i64", std::array<Token, 3> {
      Token(INTEGER, uint64_t(57)),
      Token(AS),
      Token(IDENTIFIER, "i64")
      });

  TestLex("0..", std::array<Token, 2> {
      Token(INTEGER, uint64_t(0)),
      Token(DOTDOT)
      });
}
