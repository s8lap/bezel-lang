#pragma once

// Test support for the lexer milestone: drive the lexer over a source string
// and assert the resulting token stream. Kept separate from the lexer so the
// tests read as a translation of the spec, not of the implementation.

#include <cstddef>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "lexer/Lexer.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

namespace bezel::test {

// Lex an entire source string into a token vector. The vector always ends with
// the terminal EndOfFile token. The lexer terminates the stream itself, so this
// cannot loop forever on a well-behaved lexer.
inline std::vector<Token> lexAll(std::string_view source,
                                 DiagnosticEngine& diagnostics) {
    Lexer lexer(source, diagnostics);
    std::vector<Token> tokens;
    for (;;) {
        Token token = lexer.next();
        tokens.push_back(token);
        if (token.kind == TokenKind::EndOfFile) {
            break;
        }
    }
    return tokens;
}

inline std::vector<Token> lexAll(std::string_view source) {
    DiagnosticEngine diagnostics;
    return lexAll(source, diagnostics);
}

struct ExpectedToken {
    TokenKind kind;
    std::string_view text;
};

// Assert that `actual` is exactly `expected` followed by a terminal EndOfFile
// token. Bounds-safe: a stub lexer that returns fewer tokens records assertion
// failures rather than reading out of range.
inline void expectTokens(const std::vector<Token>& actual,
                         std::initializer_list<ExpectedToken> expected) {
    BEZEL_EXPECT_EQ(actual.size(), expected.size() + 1);
    std::size_t i = 0;
    for (const ExpectedToken& e : expected) {
        if (i >= actual.size()) {
            break;
        }
        BEZEL_EXPECT_TRUE(actual[i].kind == e.kind);
        BEZEL_EXPECT_TRUE(actual[i].text == e.text);
        ++i;
    }
    if (!actual.empty()) {
        BEZEL_EXPECT_TRUE(actual.back().kind == TokenKind::EndOfFile);
    }
}

} // namespace bezel::test
