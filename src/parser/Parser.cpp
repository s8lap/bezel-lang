#include "parser/Parser.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace bezel {

namespace {

// §3.4: the lowercase primitive-type aliases. These are unconditionally
// reserved keywords (Appendix B.1) but they ARE valid type-name first
// components — `int`, `string`, etc. The PascalCase forms (`Object`, `String`,
// `Array`) are ordinary identifiers and need no special handling here.
bool isPrimitiveTypeKeyword(std::string_view text) {
    return text == "bool"   || text == "byte"  || text == "sbyte" ||
           text == "short"  || text == "ushort" || text == "int"  ||
           text == "uint"   || text == "long"  || text == "ulong" ||
           text == "float"  || text == "double" || text == "decimal" ||
           text == "char"   || text == "string";
}

} // namespace

Parser::Parser(Lexer& lexer, DiagnosticEngine& diagnostics)
    : lexer_(lexer), diagnostics_(diagnostics) {
    advance(); // prime current_ with the first token
}

// §18.5 / §18.7: split an interpolated string token into its alternating
// literal and expression parts.  The lexer has already validated balanced
// braces, so we can walk the content in a single pass:
//  - literal chars accumulate into `current_literal`
//  - `{` opens an expression region; we find the matching `}` and create a
//    sub-parser to parse one expression from that slice
//  - `{{` / `}}` are literal-brace escapes; verbatim strings skip all `\`
//  - `\` in non-verbatim strings is consumed as an escape sequence
void Parser::splitInterpolation(ast::InterpolatedStringExpr& e) {
    std::string_view raw = e.text;   // full `$"..."` or `$@"..."` token text
    // Determine where the content starts and ends.
    size_t start = e.isVerbatim ? 3 : 2;  // skip `$@"` or `$"`
    if (raw.size() < start + 1) return;
    size_t end = raw.size() - 1;          // before the closing `"`

    std::string literal;
    size_t i = start;
    while (i < end) {
        char c = raw[i];
        // Literal-brace escapes `{{` → `{` and `}}` → `}`.
        if (c == '{' && i + 1 < end && raw[i + 1] == '{') {
            literal.push_back('{');
            i += 2;
            continue;
        }
        if (c == '}' && i + 1 < end && raw[i + 1] == '}') {
            literal.push_back('}');
            i += 2;
            continue;
        }
        // Expression region `{expr}`.
        if (c == '{') {
            ++i; // past `{`
            // Find matching `}` tracking brace depth (and nested strings).
            size_t exprStart = i;
            int depth = 1;
            while (i < end && depth > 0) {
                char ic = raw[i];
                if (ic == '"') {
                    // Nested string — skip to its closing `"`.
                    ++i;
                    while (i < end && raw[i] != '"') {
                        if (!e.isVerbatim && raw[i] == '\\') ++i; // escape
                        ++i;
                    }
                    if (i < end) ++i; // closing `"`
                } else if (ic == '{') { ++depth; ++i; }
                else if (ic == '}') { --depth; if (depth > 0) ++i; else break; }
                else ++i;
            }
            // `raw[exprStart..i)` is the expression text.
            std::string_view exprText = raw.substr(exprStart, i - exprStart);
            if (i < end) ++i; // consume `}`

            // Flush any accumulated literal.
            if (!literal.empty()) {
                ast::InterpPart lp;
                lp.literal = std::move(literal);
                e.parts.push_back(std::move(lp));
                literal.clear();
            }
            // Sub-parse the expression. Check for a format specifier
            // `{expr:FORMAT}` (§18.5): a `:` not inside balanced
            // delimiters splits expression from the format spec.
            // The format spec is stored in InterpPart::formatSpec and
            // applied at codegen time (avoids dangling-string_view bug
            // that would result from modifying the expression text).
            std::string_view actualExpr = exprText;
            std::string fmtSpec;
            {
                int d = 0; // paren/bracket depth
                for (size_t k = 0; k < exprText.size(); ++k) {
                    char ek = exprText[k];
                    if (ek == '(' || ek == '[') ++d;
                    else if (ek == ')' || ek == ']') --d;
                    else if (d == 0 && k + 1 < exprText.size()) {
                        // `:FORMAT` — standard format specifier.
                        if (ek == ':') {
                            // §18.5 (#1099A): the format spec is everything after
                            // this first depth-0 `:` (Bezel has no ternary `:`, so
                            // a `:` outside brackets is always the format
                            // separator). Besides alphanumerics and sign, a custom
                            // date/time format carries separators — `:` (HH:mm:ss),
                            // `/` (yyyy/MM/dd), `.` (fff), and spaces (a combined
                            // `yyyy-MM-dd HH:mm:ss`). Rejecting those left the colon
                            // form mis-parsed as the expression (the spec was
                            // discarded → no-arg ToString).
                            bool looksLikeFmt = true;
                            for (size_t f = k + 1; f < exprText.size(); ++f) {
                                char fc = exprText[f];
                                if (!std::isalnum(static_cast<unsigned char>(fc)) &&
                                    fc != '-' && fc != '+' && fc != ':' &&
                                    fc != '/' && fc != '.' && fc != ' ')
                                    { looksLikeFmt = false; break; }
                            }
                            if (looksLikeFmt) {
                                actualExpr = exprText.substr(0, k);
                                fmtSpec = std::string(exprText.substr(k + 1));
                            }
                            break;
                        }
                        // `,WIDTH` or `,WIDTH:FORMAT` — alignment specifier.
                        // Negative WIDTH = left-align.
                        // e.g. `{x,-15}`, `{x,10}`, `{x,10:F2}`.
                        if (ek == ',') {
                            auto tail = exprText.substr(k + 1);
                            bool looksLikeWidth = !tail.empty();
                            size_t start = 0;
                            if (!tail.empty() && (tail[0] == '-' || tail[0] == '+'))
                                start = 1;
                            size_t digitEnd = start;
                            while (digitEnd < tail.size() && std::isdigit(tail[digitEnd]))
                                ++digitEnd;
                            if (digitEnd == start) looksLikeWidth = false; // no digits
                            // After the digits, only `}` (end) or `:FORMAT` is valid.
                            if (looksLikeWidth && digitEnd < tail.size() &&
                                tail[digitEnd] != ':')
                                looksLikeWidth = false;
                            if (looksLikeWidth) {
                                actualExpr = exprText.substr(0, k);
                                // Encode as "," + WIDTH (+ optional ":FORMAT").
                                fmtSpec = "," + std::string(tail);
                            }
                            break;
                        }
                    }
                }
            }
            // §18.5 (#746 R41-G): `{expression}` requires an expression; an
            // empty hole `{}` (or whitespace-only / `{:FORMAT}` with no
            // expression) was silently accepted as an empty contribution,
            // masking a missing-expression typo. Reject it.
            {
                bool blank = true;
                for (char bc : actualExpr)
                    if (!std::isspace(static_cast<unsigned char>(bc))) {
                        blank = false;
                        break;
                    }
                if (blank)
                    diagnostics_.report(
                        Severity::Error, e.span.begin,
                        "empty interpolation hole — `{...}` requires an "
                        "expression (§18.5)");
            }
            if (!actualExpr.empty()) {
                // Heap-allocate the source string so string_views inside the
                // sub-parsed expression AST remain valid through vector moves
                // (avoids SSO invalidation when the InterpPart is moved).
                auto srcOwned =
                    std::make_unique<std::string>(std::string(actualExpr));
                Lexer subLex(*srcOwned, diagnostics_);
                Parser subPar(subLex, diagnostics_);
                auto expr = subPar.parseExpression();
                if (expr) {
                    ast::InterpPart ep;
                    ep.expr = std::move(expr);
                    ep.formatSpec = std::move(fmtSpec);
                    ep.exprSource = std::move(srcOwned);
                    e.parts.push_back(std::move(ep));
                }
            }
            continue;
        }
        // Non-verbatim escape: skip the escape char, let processStringText
        // handle the actual byte at codegen time — just copy the raw chars.
        if (!e.isVerbatim && c == '\\' && i + 1 < end) {
            literal.push_back(c);
            literal.push_back(raw[i + 1]);
            i += 2;
            continue;
        }
        literal.push_back(c);
        ++i;
    }
    // Trailing literal.
    if (!literal.empty()) {
        ast::InterpPart lp;
        lp.literal = std::move(literal);
        e.parts.push_back(std::move(lp));
    }
}

void Parser::advance() {
    if (!peeked_.empty()) {
        current_ = std::move(peeked_.front());
        peeked_.pop_front();
    } else {
        current_ = lexer_.next();
    }
}

const Token& Parser::peekAt(std::size_t k) {
    // k=0 returns the current token; k>=1 reads from the buffered lookahead,
    // filling it lazily from the lexer. Cheap for the small k typically
    // needed; the discriminator for `(…) => …` may scan to the matching `)`
    // before deciding lambda-vs-paren.
    if (k == 0) return current_;
    while (peeked_.size() < k) {
        peeked_.push_back(lexer_.next());
    }
    return peeked_[k - 1];
}

bool Parser::isKeyword(std::string_view text) const {
    return current_.kind == TokenKind::Keyword && current_.text == text;
}

bool Parser::isPunctuator(std::string_view text) const {
    return current_.kind == TokenKind::Punctuator && current_.text == text;
}

bool Parser::consumeKeyword(std::string_view text) {
    if (!isKeyword(text)) return false;
    advance();
    return true;
}

bool Parser::consumePunctuator(std::string_view text) {
    if (!isPunctuator(text)) return false;
    advance();
    return true;
}

// §1.6 / v0.170 (#1374): uniform end-label for keyword-labelled constructs.
// The keyword after `end` is optional; when present it must match the
// construct's introducing keyword. If a different keyword from the known
// end-label set appears, a mismatch diagnostic is emitted and it is consumed
// so the `;` terminator can still be found.
void Parser::consumeOptionalEndKeyword(std::string_view expected) {
    if (current_.kind != TokenKind::Keyword) return; // bare `end;` — always valid
    if (current_.text == expected) {
        advance(); // label matches
        return;
    }
    // Only report a mismatch when the token is plausibly an end-label attempt
    // (i.e. it belongs to the known set of construct-introducing keywords that
    // are valid end-labels). Other keywords are left for the caller so that
    // `end` followed by the next statement's keyword does not produce a spurious
    // diagnostic.
    static constexpr std::string_view kEndLabelKws[] = {
        "if", "while", "for", "do", "try",
        "case", "when", "extension", "extern",
    };
    for (auto kw : kEndLabelKws) {
        if (current_.text == kw) {
            std::string msg = "end label '";
            msg += current_.text;
            msg += "' does not match the construct keyword '";
            msg += expected;
            msg += "'";
            diagnostics_.report(Severity::Error, current_.location, std::move(msg));
            advance(); // consume for error recovery
            return;
        }
    }
    // Not a recognised end-label keyword — leave it untouched.
}

} // namespace bezel
