//===-- RustLex.h -------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_RustLex_h
#define liblldb_RustLex_h

#include <memory>

#include "lldb/lldb-forward.h"
#include "lldb/lldb-private.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "lldb/Utility/Stream.h"

namespace lldb_private {

namespace rust {

// Note that single-character tokens are represented by the character
// itself.
enum TokenKind {
  STRING = 128,
  BYTESTRING,
  CHAR,
  BYTE,
  FLOAT,
  INTEGER,
  AS,
  TRUE,
  FALSE,
  SUPER,
  SELF,
  MUT,
  CONST,
  FN,
  SIZEOF,
  DOTDOT,
  DOTDOTEQ,
  OROR,
  ANDAND,
  EQEQ,
  NOTEQ,
  LTEQ,
  GTEQ,
  LSH,
  RSH,
  PLUS_EQ,
  MINUS_EQ,
  SLASH_EQ,
  STAR_EQ,
  PERCENT_EQ,
  RSH_EQ,
  LSH_EQ,
  AND_EQ,
  OR_EQ,
  XOR_EQ,
  COLONCOLON,
  ARROW,
  IDENTIFIER,
  INVALID,
  THATSALLFOLKS
};

void PrintTokenKind(Stream &stream, int kind);

struct Token {
  int kind;

  llvm::Optional<uint64_t> uinteger;
  llvm::Optional<double> dvalue;
  // This can be NULL if no suffix was specified.
  const char *number_suffix = nullptr;
  std::string str;

  explicit Token(int kind_)
    : kind(kind_)
  {
  }

  Token(int kind_, uint64_t val_, const char *suffix_ = nullptr)
    : kind(kind_),
      uinteger(val_),
      number_suffix(suffix_)
  {
  }

  Token(int kind_, double val_, const char *suffix_ = nullptr)
    : kind(kind_),
      dvalue(val_),
      number_suffix(suffix_)
  {
  }

  Token(int kind_, std::string &&str_)
    : kind(kind_),
      str(std::move(str_))
  {
  }

  bool operator==(const Token &other) const {
    if (kind != other.kind ||
        uinteger != other.uinteger ||
        dvalue != other.dvalue ||
        str != other.str) {
      return false;
    }

    if (number_suffix == nullptr) {
      return other.number_suffix == nullptr;
    }
    if (other.number_suffix == nullptr) {
      return false;
    }

    return strcmp(number_suffix, other.number_suffix) == 0;
  }
};

class Lexer {
public:

  explicit Lexer(llvm::StringRef ref)
    : m_iter(ref.begin()),
      m_end(ref.end())
  {
  }

  Token Next();

private:

  const char *CheckSuffix(const char *const *suffixes);
  bool BasicInteger(int *radix_out, std::string *value);
  Token MaybeByteLiteral();
  Token MaybeRawString(bool is_byte = false);
  Token String(bool is_byte = false);
  Token Character(bool is_byte = false);
  Token Operator();
  int Float(std::string *value);
  Token Number();
  Token Identifier();
  bool AppendEscape(std::string *result, bool is_byte);
  bool ParseHex(uint64_t *result, int min_digits, int max_digits);
  bool ParseEscape(uint64_t *result, bool is_byte);
  bool Lookup(const ::llvm::StringRef &str, int *result);

  llvm::StringRef::iterator m_iter;
  llvm::StringRef::iterator m_end;

  size_t Remaining() const { return m_end - m_iter; }

  static llvm::StringMap<TokenKind> *Keywords();

  static llvm::StringMap<TokenKind> *m_keywords;
};

} // namespace rust

} // namespace lldb_private

#endif // liblldb_RustLex_h
