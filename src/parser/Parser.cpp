#include "parser/Parser.hpp"
#include "parser/Parser_impl.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace bezel {

namespace {

// Best-effort recognizer for the §18.5 hole-delimitation sharp edge (#2102):
// an unparenthesized generic-argument list at the top level of a hole —
// `Identifier<T1, T2>.Member` / `Identifier<T1, T2>(args)`, the Appendix A.7
// `Identifier GenericArgs` atom, disambiguated the same way §14.1/§22.12
// disambiguate it everywhere else it appears (`Parser::tokensLookLikeReceiverTypeArgs`
// / `Parser::tokensLookLikeMethodTypeArgs`: a bare `<` right after a name,
// followed only by TypeArgToken-shaped tokens, closing on a bare `>`/`>>`
// immediately followed by `.` or `(`) — has its top-level comma or colon cut
// by the normative hole-delimitation rule (§18.5, v0.172, #1339). That rule's
// tracked nesting set is `()[]{}` only and does not extend to `<...>`: this is
// the CURRENT spec text, read literally, and is not changed here (`<` doubles
// as the relational/`<>`/`<<` operator family, and generic-vs-relational is
// only resolvable with this same lookahead — seeing this as a plain
// character-nesting rule like the others is exactly the tension #2102 records
// and defers to a spec ruling). So this recognizer does NOT change which
// programs compile — the cut, and the resulting error, remain exactly today's
// spec-conformant behavior. It exists purely so a clear, on-point diagnostic
// can be raised at the cut, instead of leaving a confusingly-unrelated
// downstream type error (e.g. "operator '<' is not defined for type ...") to
// describe the mistake alone.
//
// Re-lexes `exprText` with a throwaway Lexer/DiagnosticEngine (discarded —
// this is advisory only and must never influence real diagnostics or the
// actual split) and reports whether `cutOffset` (the byte offset, within
// exprText, of the `,`/`:` the real rule cut on) falls strictly inside a
// confirmed generic-argument-list's token span.
bool holeCutInsideGenericArgList(std::string_view exprText,
                                  std::size_t cutOffset) {
    DiagnosticEngine scratchDiag;
    Lexer lex(exprText, scratchDiag);
    std::vector<Token> toks;
    for (;;) {
        Token t = lex.next();
        bool atEnd = t.kind == TokenKind::EndOfFile;
        toks.push_back(t);
        if (atEnd) break;
    }
    auto isName = [](const Token& t) {
        return t.kind == TokenKind::Identifier ||
               (t.kind == TokenKind::Keyword && isPrimitiveTypeKeyword(t.text));
    };
    auto isTypeArgToken = [](const Token& t) {
        if (t.kind == TokenKind::Identifier) return true;
        if (t.kind == TokenKind::Keyword) return isPrimitiveTypeKeyword(t.text);
        if (t.kind == TokenKind::Punctuator)
            return t.text == "," || t.text == "?" ||
                   t.text == "[" || t.text == "]";
        return false;
    };
    for (std::size_t i = 1; i + 1 < toks.size(); ++i) {
        const Token& open = toks[i];
        if (open.kind != TokenKind::Punctuator || open.text != "<") continue;
        if (!isName(toks[i - 1])) continue;  // §A.7: needs an `Identifier` receiver

        std::size_t depth = 1;
        std::size_t j = i + 1;
        bool closed = false;
        for (; j < toks.size(); ++j) {
            const Token& u = toks[j];
            if (u.kind == TokenKind::EndOfFile) break;
            if (u.kind == TokenKind::Punctuator && u.text == "<") {
                ++depth;
                continue;
            }
            if (u.kind == TokenKind::Punctuator && u.text == ">>") {
                // Mirrors tokensLookLikeMethodTypeArgs: `>>` closes two
                // levels at once; closing exactly the outstanding two is a
                // match, closing just one (depth==1) is malformed (bail).
                if (depth <= 2) {
                    if (depth == 2) closed = true;
                    break;
                }
                depth -= 2;
                continue;
            }
            if (u.kind == TokenKind::Punctuator && u.text == ">") {
                if (--depth == 0) { closed = true; break; }
                continue;
            }
            if (!isTypeArgToken(u)) break;  // not a type-arg list after all
        }
        if (!closed) continue;
        // The §18.5 (v0.226, #2139) case: a bare `Identifier<T1, T2>` at the
        // hole's top level — the `>` is the LAST token of the fragment (no
        // `.Member` / `(...)` access follows). The cut on `,` still happened
        // mid-generic-args; the §18.5 rule applies, and the fragment now
        // contains an unmatched `<`. Without this branch, the recognizer
        // would reject the case and the user would see a confusing downstream
        // type error ("operator `<` is not defined", "unresolved identifier
        // `x`", etc.) pointing at the wrong construct. (A relational chain
        // like `a < b, c > d` keeps a non-EOF token after the `>` and so
        // still falls through to the `followedByAccess` check.)
        const std::size_t afterIdx = j + 1;
        const bool afterIsEof = afterIdx >= toks.size() ||
            toks[afterIdx].kind == TokenKind::EndOfFile;
        if (afterIsEof) {
            if (cutOffset > open.location.offset &&
                cutOffset < toks[j].location.offset + toks[j].text.size())
                return true;
            continue;
        }
        const Token& after = toks[afterIdx];
        bool followedByAccess = after.kind == TokenKind::Punctuator &&
            (after.text == "." || after.text == "(");
        if (!followedByAccess) continue;

        std::size_t spanStart = open.location.offset;
        std::size_t spanEnd = toks[j].location.offset + toks[j].text.size();
        if (cutOffset > spanStart && cutOffset < spanEnd) return true;
    }
    return false;
}

// §18.5 (v0.306, #4517): compute the SourceLocation of `text[uptoOffset]`
// given that `text[0]` sits at `base` in the enclosing file. A hole's
// expression text is re-lexed from a standalone copy (see splitInterpolation
// below), so its own Lexer's locations start fresh at {1, 1, 0} unless
// rebased — before this, a diagnostic anywhere inside a hole (e.g. "expected
// expression" on `{if cmp < 0 then ... }` missing its mandatory outer parens)
// printed as the file's `1:1` instead of pointing at the hole. Mirrors
// Lexer::locationAt's walk (CR/CRLF/LF each advance the line once and reset
// the column; every other byte advances the column), just parameterized over
// an arbitrary starting location instead of always the file origin.
SourceLocation advanceLocation(SourceLocation base, std::string_view text,
                                std::size_t uptoOffset) {
    std::uint32_t line = base.line;
    std::uint32_t column = base.column;
    const std::size_t limit = uptoOffset < text.size() ? uptoOffset : text.size();
    for (std::size_t i = 0; i < limit; ++i) {
        char c = text[i];
        if (c == '\r') {
            ++line;
            column = 1;
            if (i + 1 < limit && text[i + 1] == '\n') ++i; // CRLF is one line break
        } else if (c == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return SourceLocation{line, column,
                          static_cast<std::uint32_t>(base.offset + uptoOffset)};
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
            // Sub-parse the expression, delimiting the alignment `,` and the
            // format-spec `:` per the normative §18.5 hole-delimitation rule
            // (v0.172, #1339): both are recognized ONLY at the hole's top
            // nesting level. The scan tracks `()` `[]` `{}` depth and skips
            // nested string/char literals (their `,`/`:`/braces do not
            // delimit). The FIRST top-level `,` begins the alignment; the
            // FIRST top-level `:` begins the format spec (Bezel has no other
            // top-level colon meaning inside a hole expression, so no
            // heuristic is needed — the earlier "looks like a format" guard is
            // gone). The format spec runs to the hole's closing `}` and is
            // brace-free (a `{` in it is a compile error). The spec is stored in
            // InterpPart::formatSpec (encoded as [","WIDTH][":"FORMAT]) and
            // applied at codegen time (avoids a dangling-string_view bug from
            // rewriting the expression text).
            std::string_view actualExpr = exprText;
            std::string fmtSpec;
            {
                int d = 0; // paren/bracket/brace depth
                std::size_t cutOffset = std::string_view::npos;
                char cutChar = '\0';
                for (size_t k = 0; k < exprText.size(); ++k) {
                    char ek = exprText[k];
                    // Skip a nested string literal as a unit.
                    if (ek == '"') {
                        ++k;
                        while (k < exprText.size() && exprText[k] != '"') {
                            if (!e.isVerbatim && exprText[k] == '\\' &&
                                k + 1 < exprText.size())
                                ++k;
                            ++k;
                        }
                        continue;
                    }
                    // Skip a nested char literal as a unit.
                    if (ek == '\'') {
                        ++k;
                        while (k < exprText.size() && exprText[k] != '\'') {
                            if (exprText[k] == '\\' && k + 1 < exprText.size()) ++k;
                            ++k;
                        }
                        continue;
                    }
                    if (ek == '(' || ek == '[' || ek == '{') { ++d; continue; }
                    if (ek == ')' || ek == ']' || ek == '}') { --d; continue; }
                    if (d != 0) continue;
                    // Top-level `:` begins the format spec (everything after it).
                    if (ek == ':') {
                        actualExpr = exprText.substr(0, k);
                        fmtSpec = std::string(exprText.substr(k + 1));
                        cutOffset = k;
                        cutChar = ':';
                        break;
                    }
                    // Top-level `,` begins the alignment; `,WIDTH[:FORMAT]`.
                    if (ek == ',') {
                        actualExpr = exprText.substr(0, k);
                        fmtSpec = "," + std::string(exprText.substr(k + 1));
                        cutOffset = k;
                        cutChar = ',';
                        break;
                    }
                }
                // §18.5 (v0.172, #1339) sharp edge (#2102): the hole-delimitation
                // rule's tracked nesting set is `()[]{}` only, so a top-level
                // `,`/`:` inside an unparenthesized generic-argument list
                // (`Identifier<T1, T2>.Member` / `(...)`) is still cut here —
                // that acceptance behavior is unchanged. When it is, name the
                // actual construct instead of leaving a downstream, unrelated
                // type error (e.g. an `operator '<' is not defined` on the
                // truncated fragment) to describe the mistake alone.
                if (cutOffset != std::string_view::npos &&
                    holeCutInsideGenericArgList(exprText, cutOffset)) {
                    diagnostics_.report(
                        Severity::Error, e.span.begin,
                        std::string("interpolation hole's `") + cutChar +
                        "` falls inside what looks like an unparenthesized "
                        "generic-argument list (`Identifier<...>`) rather than "
                        "starting the alignment/format spec — \xC2\xA7"
                        "18.5's hole-delimitation rule nests `()[]{}` only, not "
                        "`<...>`; parenthesize the expression, e.g. "
                        "`{(Expr<T1, T2>.Member)}`, so the whole call sits "
                        "inside the hole's top nesting level (\xC2\xA7"
                        "18.5, v0.172, #1339)");
                }
                // §18.5 (v0.172, #1339): the format spec is brace-free — a `{`
                // in it is a compile error. Check the FORMAT portion (after any
                // `,`WIDTH alignment prefix).
                std::string_view fmtPortion = fmtSpec;
                if (!fmtPortion.empty() && fmtPortion.front() == ',') {
                    size_t colon = fmtPortion.find(':');
                    fmtPortion = (colon == std::string_view::npos)
                        ? std::string_view()
                        : fmtPortion.substr(colon + 1);
                }
                if (fmtPortion.find('{') != std::string_view::npos)
                    diagnostics_.report(
                        Severity::Error, e.span.begin,
                        "a `{` may not appear in an interpolation format spec — "
                        "the spec runs to the hole's closing `}` (\xC2\xA7""18.5)");
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
                // §18.5 (v0.306, #4517): rebase the sub-lexer onto the hole's
                // real position in the file (raw[exprStart] is the first
                // character of the expression, right after the opening `{`),
                // so a diagnostic from inside the hole (e.g. a missing
                // mandatory-parenthesization "expected expression") points at
                // the hole instead of the file's 1:1.
                SourceLocation holeExprBase =
                    advanceLocation(e.span.begin, raw, exprStart);
                Lexer subLex(*srcOwned, diagnostics_, holeExprBase);
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

// §1.3 / §17.13 / A.1 / B.1 (v0.244, proposal 0083; #3049). The bare `_` is a
// reserved TOKEN — "held out of the identifier namespace like a punctuation
// symbol" (B.1) — so §17.13's rejected-position list is grammatical, not a
// name-resolution rule: "every declaration named `_` — a method or constructor
// parameter …, a field, property, method, type, module, enum constant, choice
// case, loop label …, or alias" is a compile error "whose diagnostic names the
// sanctioned forms". This is that diagnostic; each caller supplies the slot
// name and, where the position has one, its specific remedy.
void Parser::reportDiscardNotDeclarable(std::string_view what,
                                        std::string_view remedy) {
    std::string msg = "`_` is the reserved discard token and cannot be used as a ";
    msg += what;
    msg += " name — it is never declarable and never readable as a name "
           "(§1.3, §17.13)";
    if (remedy.empty()) {
        msg += "; choose a name";
    } else {
        msg += "; ";
        msg += remedy;
    }
    diagnostics_.report(Severity::Error, current_.location, std::move(msg));
    advance();  // consume the `_` so one token does not cascade
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

void Parser::consumeOptionalLoopEndLabel(std::string_view keyword,
                                         std::string_view loopLabel) {
    // §1.6 / §21.5 (v0.201): closer rules for loop statements. An Identifier is
    // treated as a closer-label attempt only when a matching label OR when it is
    // immediately followed by `;` — otherwise it is left for the caller so a
    // missing-semicolon (`… end` then the next statement) keeps its old recovery.
    const bool identCloser =
        current_.kind == TokenKind::Identifier &&
        (current_.text == loopLabel ||
         (peekNext().kind == TokenKind::Punctuator && peekNext().text == ";"));
    if (!loopLabel.empty()) {
        // Named construct: `end scan;` or bare `end;`.
        if (identCloser) {
            if (current_.text == loopLabel) {
                advance(); // label matches
            } else {
                std::string msg = "end label '";
                msg += current_.text;
                msg += "' does not match the loop label '";
                msg += loopLabel;
                msg += "'; close with 'end ";
                msg += loopLabel;
                msg += ";' or 'end;'";
                diagnostics_.report(Severity::Error, current_.location,
                                    std::move(msg));
                advance(); // consume for error recovery
            }
            return;
        }
        // The introducing keyword closer does NOT apply to a named loop.
        if (current_.kind == TokenKind::Keyword && current_.text == keyword) {
            std::string msg = "this loop is named '";
            msg += loopLabel;
            msg += "'; close with 'end ";
            msg += loopLabel;
            msg += ";' or 'end;', not 'end ";
            msg += keyword;
            msg += ";' (§1.6/§21.5)";
            diagnostics_.report(Severity::Error, current_.location,
                                std::move(msg));
            advance(); // consume for error recovery
            return;
        }
        // Bare `end;` (a `;` or anything else) — always valid; leave untouched.
        return;
    }
    // Unlabeled loop: `end for;`/`end while;` or bare `end;`.
    if (current_.kind == TokenKind::Keyword && current_.text == keyword) {
        advance(); // keyword closer matches
        return;
    }
    // An identifier closer on an unlabeled loop is a mismatch — the loop has no
    // name to repeat (§1.6). Report only when it is clearly a closer attempt.
    if (identCloser) {
        std::string msg = "end label '";
        msg += current_.text;
        msg += "' does not match: this '";
        msg += keyword;
        msg += "' loop is unlabeled — close with 'end ";
        msg += keyword;
        msg += ";' or 'end;'";
        diagnostics_.report(Severity::Error, current_.location, std::move(msg));
        advance(); // consume for error recovery
        return;
    }
    // A mismatched keyword closer (e.g. `end while;` on a `for`): reuse the
    // known-keyword mismatch diagnostic path.
    consumeOptionalEndKeyword(keyword);
}

} // namespace bezel
