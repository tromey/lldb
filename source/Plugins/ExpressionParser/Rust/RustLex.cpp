//===-- RustLex.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "RustLex.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/ADT/APInt.h"

using namespace lldb_private::rust;
using namespace lldb_private;
using namespace lldb;
using namespace llvm;

void lldb_private::rust::PrintTokenKind(Stream &stream, int kind) {
  if (kind < STRING) {
    stream << char(kind);
  } else {
    switch (kind) {
    case STRING:
    case BYTESTRING:
    case CHAR:
    case BYTE:
    case FLOAT:
    case INTEGER:
    case AS:
    case TRUE:
    case FALSE:
    case SUPER:
    case SELF:
    case MUT:
    case CONST:
    case FN:
    case SIZEOF:
    case IDENTIFIER:
    case INVALID:
    case THATSALLFOLKS:
      // May want to clean this up someday.
      stream << "[TOKEN=" << kind << "]";
      break;

    case DOTDOT:
      stream << "..";
      break;

    case DOTDOTEQ:
      stream << "..=";
      break;

    case OROR:
      stream << "||";
      break;

    case ANDAND:
      stream << "&&";
      break;

    case EQEQ:
      stream << "==";
      break;

    case NOTEQ:
      stream << "!=";
      break;

    case LTEQ:
      stream << "<=";
      break;

    case GTEQ:
      stream << ">=";
      break;

    case LSH:
      stream << "<<";
      break;

    case RSH:
      stream << ">>";
      break;

    case PLUS_EQ:
      stream << "+=";
      break;

    case MINUS_EQ:
      stream << "-=";
      break;

    case SLASH_EQ:
      stream << "/=";
      break;

    case STAR_EQ:
      stream << "*=";
      break;

    case PERCENT_EQ:
      stream << "%=";
      break;

    case RSH_EQ:
      stream << ">>=";
      break;

    case LSH_EQ:
      stream << "<<=";
      break;

    case AND_EQ:
      stream << "&=";
      break;

    case OR_EQ:
      stream << "|=";
      break;

    case XOR_EQ:
      stream << "^=";
      break;

    case COLONCOLON:
      stream << "::";
      break;

    case ARROW:
      stream << "->";
      break;

    default:
      stream << "!!!OOPS!!!";
      break;
    }
  }
}

llvm::StringMap<TokenKind> *Lexer::m_keywords;

Token Lexer::Next() {
  // Skip whitespace.
  while (m_iter != m_end &&
         // FIXME is it possible to see newlines here?
         (*m_iter == ' ' || *m_iter == '\t'))
    ++m_iter;
  if (m_iter == m_end) {
    return Token(THATSALLFOLKS);
  }

  char c = *m_iter;
  if (c >= '0' && c <= '9') {
    return Number();
  } else if (c == 'b') {
    return MaybeByteLiteral();
  } else if (c == 'r') {
    return MaybeRawString();
  } else if (c == '"') {
    return String();
  } else if (c == '\'') {
    return Character();
  } else {
    return Operator();
  }
}

bool Lexer::Lookup(const ::llvm::StringRef &str, int *result) {
  ::llvm::StringMap<TokenKind> *map = Keywords();
  const auto &iter = map->find(str);
  if (iter == map->end()) {
    return false;
  }
  *result = iter->second;
  return true;
}

Token Lexer::Operator() {
  int result;

  if (Remaining() >= 3 && Lookup(::llvm::StringRef(m_iter, 3), &result)) {
    m_iter += 3;
    return Token(result);
  }

  if (Remaining() >= 2 && Lookup(::llvm::StringRef(m_iter, 2), &result)) {
    m_iter += 2;
    return Token(result);
  }

  if (strchr(".,;|&=!<>+-*/%:[](){}", *m_iter) != nullptr) {
    return Token(*m_iter++);
  }

  return Identifier();
}

bool Lexer::BasicInteger(int *radix_out, std::string *value) {
  int radix = 10;

  assert (m_iter != m_end);
  assert (*m_iter >= '0' && *m_iter <= '9');

  bool need_digit = false;
  if (radix_out != nullptr && *m_iter == '0') {
    // Ignore this digit and see if we have a non-decimal integer.
    ++m_iter;
    if (m_iter == m_end) {
      // Plain "0".
      value->push_back('0');
      if (radix_out) {
        *radix_out = radix;
      }
      return true;
    }

    if (*m_iter == 'x') {
      radix = 16;
      need_digit = true;
      ++m_iter;
    } else if (*m_iter == 'b') {
      radix = 2;
      need_digit = true;
      ++m_iter;
    } else if (*m_iter == 'o') {
      radix = 8;
      need_digit = true;
      ++m_iter;
    } else {
      value->push_back('0');
    }
  }

  for (; m_iter != m_end; ++m_iter) {
    if (*m_iter == '_') {
      continue;
    }
    if ((radix == 10 || radix == 16) && *m_iter >= '0' && *m_iter <= '9') {
      // Ok.
    } else if (radix == 2 && *m_iter >= '0' && *m_iter <= '1') {
      // Ok.
    } else if (radix == 8 && *m_iter >= '0' && *m_iter <= '7') {
      // Ok.
    } else if (radix == 16 && *m_iter >= 'a' && *m_iter <= 'f') {
      // Ok.
    } else if (radix == 16 && *m_iter >= 'A' && *m_iter <= 'F') {
      // Ok.
    } else {
      break;
    }

    value->push_back(*m_iter);
    need_digit = false;
  }

  if (radix_out) {
    *radix_out = radix;
  }
  return !need_digit;
}

const char *Lexer::CheckSuffix(const char *const *suffixes) {
  const char *suffix = nullptr;

  size_t left = Remaining();
  for (int i = 0; suffixes[i]; ++i) {
    size_t len = strlen(suffixes[i]);
    if (left >= len) {
      ::llvm::StringRef text(m_iter, len);
      if (text == suffixes[i]) {
        suffix = suffixes[i];
        m_iter += len;
        break;
      }
    }
  }

  return suffix;
}

int Lexer::Float(std::string *value) {
  assert(m_iter != m_end && (*m_iter == '.' || *m_iter == 'e' || *m_iter == 'E'));

  if (*m_iter == '.') {
    ++m_iter;
    if (m_iter == m_end || !(*m_iter >= '0' && *m_iter <= '9')) {
      // Not a floating-point number.
      --m_iter;
      return INTEGER;
    }

    value->push_back('.');
    BasicInteger(nullptr, value);
  }

  if (m_iter == m_end || (*m_iter != 'e' && *m_iter != 'E')) {
    return FLOAT;
  }

  value->push_back(*m_iter++);
  if (m_iter == m_end) {
    return INVALID;
  }

  if (*m_iter == '+' || *m_iter == '-') {
    value->push_back(*m_iter++);
    if (m_iter == m_end) {
      return INVALID;
    }
  }

  if (!(*m_iter >= '0' && *m_iter <= '9')) {
    return INVALID;
  }
  BasicInteger(nullptr, value);
  return FLOAT;
}

Token Lexer::Number() {
  std::string number;
  int radix;

  if (!BasicInteger(&radix, &number)) {
    return Token(INVALID);
  }

  if (m_iter != m_end && radix == 10 &&
      (*m_iter == '.' || *m_iter == 'e' || *m_iter == 'E')) {
    int kind = Float(&number);
    if (kind == INVALID) {
      return Token(INVALID);
    }
    if (kind == FLOAT) {
      // Actually a float.
      ::llvm::StringRef sref(number);
      double dval;
      if (sref.getAsDouble(dval)) {
        return Token(INVALID);
      }

      static const char * const float_suffixes[] = {
        "f32",
        "f64",
        nullptr
      };

      const char *suffix = CheckSuffix(float_suffixes);

      return Token(FLOAT, dval, suffix);
    }

    // Floating-point lex failed but we still have an integer.
    assert(kind == INTEGER);
  }

  static const char * const int_suffixes[] = {
    "u8",
    "i8",
    "u16",
    "i16",
    "u32",
    "i32",
    "u64",
    "i64",
    "usize",
    "isize",
    nullptr
  };

  APInt value;
  ::llvm::StringRef sref(number);
  if (sref.getAsInteger(radix, value)) {
    return Token(INVALID);
  }
  // FIXME maybe we should just leave it as an APInt through the whole
  // process.
  if (value.getNumWords() > 1) {
    return Token(INVALID);
  }

  const char *suffix = CheckSuffix(int_suffixes);

  return Token(INTEGER, value.getLimitedValue(), suffix);
}

Token Lexer::Identifier() {
  assert(m_iter != m_end);

  ::llvm::StringRef::iterator start = m_iter;
  char c = *m_iter;
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
    return Token(INVALID);
  }

  for (++m_iter; m_iter != m_end; ++m_iter) {
    char c = *m_iter;
    if (! ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (c >= '0' && c <= '9'))) {
      break;
    }
  }

  ::llvm::StringRef text(start, m_iter - start);
  int result;
  if (Lookup(text, &result)) {
    return Token(result);
  }

  return Token(IDENTIFIER, text.str());
}

Token Lexer::MaybeByteLiteral() {
  assert(*m_iter == 'b');

  if (Remaining() < 2) {
    return Identifier();
  }
  if (m_iter[1] == 'r') {
    return MaybeRawString(true);
  } else if (m_iter[1] == '"') {
    ++m_iter;
    return String(true);
  } else if (m_iter[1] == '\'') {
    ++m_iter;
    return Character(true);
  }
  return Identifier();
}

bool Lexer::ParseHex(uint64_t *result, int min_digits, int max_digits) {
  *result = 0;
  int i;
  for (i = 0; m_iter != m_end && i < max_digits; ++i, ++m_iter) {
    uint64_t digit;
    if (*m_iter >= 'a' && *m_iter <= 'f') {
      digit = *m_iter - 'a' + 10;
    } else if (*m_iter >= 'A' && *m_iter <= 'F') {
      digit = *m_iter - 'A' + 10;
    } else if (*m_iter >= '0' && *m_iter <= '9') {
      digit = *m_iter - '0';
    } else {
      break;
    }
    *result = *result * 16 + digit;
  }

  return i >= min_digits;
}

bool Lexer::ParseEscape(uint64_t *result, bool is_byte) {
  assert(*m_iter == '\\');
  ++m_iter;

  if (m_iter == m_end) {
    return false;
  }
  switch (*m_iter++) {
  case 'x':
    return ParseHex(result, 2, 2);

  case 'u': {
    if (is_byte) {
      return false;
    }
    if (m_iter == m_end || *m_iter++ != '{') {
      return false;
    }
    if (!ParseHex(result, 1, 6)) {
      return false;
    }
    if (m_iter == m_end || *m_iter++ != '}') {
      return false;
    }
    break;
  }

  case 'n':
    *result = '\n';
    break;
  case 'r':
    *result = '\r';
    break;
  case 't':
    *result = '\t';
    break;
  case '\\':
    *result = '\\';
    break;
  case '0':
    *result = 0;
    break;
  case '\'':
    *result = '\'';
    break;
  case '"':
    *result = '"';
    break;

  default:
    return false;
  }

  return true;
}

bool Lexer::AppendEscape(std::string *result, bool is_byte) {
  uint64_t value;
  if (!ParseEscape(&value, is_byte)) {
    return false;
  }

  char utf8[10];
  char *out = utf8;
  if (!ConvertCodePointToUTF8(value, out)) {
    return false;
  }

  result->append(utf8, out);
  return true;
}

Token Lexer::Character(bool is_byte) {
  assert(*m_iter == '\'');

  if (++m_iter == m_end) {
    return Token(INVALID);
  }

  uint64_t result;
  if (*m_iter == '\\') {
    if (!ParseEscape(&result, is_byte)) {
      return Token(INVALID);
    }
  } else {
    result = *m_iter++;
  }

  if (m_iter == m_end || *m_iter++ != '\'') {
    return Token(INVALID);
  }

  return Token(is_byte ? BYTE : CHAR, result);
}

Token Lexer::MaybeRawString(bool is_byte) {
  // Use a local copy so we can backtrack if need be.
  ::llvm::StringRef::iterator iter = m_iter;

  if (is_byte) {
    assert(*iter == 'b');
    ++iter;
  }

  assert(*iter == 'r');
  ++iter;

  ::llvm::StringRef::iterator before_hashes = iter;
  while (iter != m_end && *iter == '#') {
    ++iter;
  }
  if (iter == m_end || *iter != '"') {
    return Identifier();
  }

  size_t n_hashes = iter - before_hashes;
  ::llvm::StringRef::iterator string_start = ++iter;

  for (; iter != m_end; ++iter) {
    if (*iter == '"' &&
        (n_hashes == 0 ||
         (size_t(m_end - iter + 1) > n_hashes &&
          strncmp(iter + 1, before_hashes, n_hashes) == 0))) {
      break;
    }
  }

  m_iter = iter;
  if (iter == m_end) {
    return Token(INVALID);
  }

  assert(*m_iter == '"');
  ++m_iter;
  assert(Remaining() >= n_hashes);
  m_iter += n_hashes;

  return Token(is_byte ? BYTESTRING : STRING, std::string(string_start, iter));
}

Token Lexer::String(bool is_byte) {
  assert(*m_iter == '"');
  ++m_iter;

  std::string text;
  while (m_iter != m_end && *m_iter != '"') {
    if (*m_iter == '\\') {
      if (!AppendEscape(&text, is_byte)) {
        return Token(INVALID);
      }
    } else {
      text += *m_iter++;
    }
  }

  if (m_iter == m_end) {
    return Token(INVALID);
  }
  assert(*m_iter == '"');
  ++m_iter;

  return Token(is_byte ? BYTESTRING : STRING, std::move(text));
}

::llvm::StringMap<TokenKind> *Lexer::Keywords() {
  if (m_keywords == nullptr) {
    m_keywords = new ::llvm::StringMap<TokenKind>;
    ::llvm::StringMap<TokenKind> &m = *m_keywords;

    m["as"] = AS;
    m["true"] = TRUE;
    m["false"] = FALSE;
    m["super"] = SUPER;
    m["self"] = SELF;
    m["mut"] = MUT;
    m["const"] = CONST;
    m["fn"] = FN;
    m["sizeof"] = SIZEOF;
    m[".."] = DOTDOT;
    m["..="] = DOTDOTEQ;
    m["||"] = OROR;
    m["|="] = OR_EQ;
    m["&&"] = ANDAND;
    m["&="] = AND_EQ;
    m["^="] = XOR_EQ;
    m["=="] = EQEQ;
    m["!="] = NOTEQ;
    m["<="] = LTEQ;
    m[">="] = GTEQ;
    m["<<"] = LSH;
    m[">>"] = RSH;
    m["+="] = PLUS_EQ;
    m["-="] = MINUS_EQ;
    m["*="] = STAR_EQ;
    m["/="] = SLASH_EQ;
    m["%="] = PERCENT_EQ;
    m["<<="] = LSH_EQ;
    m[">>="] = RSH_EQ;
    m["::"] = COLONCOLON;
    m["->"] = ARROW;
  }

  return m_keywords;
}
