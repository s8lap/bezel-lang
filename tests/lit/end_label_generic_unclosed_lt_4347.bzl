// fix #4347 (fourth instance of the discard-loop-swallow class that issues
// #4257 / #4341 / #4343 fixed earlier this run; Appendix A.3 / §1.6,
// v0.298).
//
// `Parser::parseTypeDeclBody`'s `end TypeName<...>;` end-label generic-
// parameter skip (the block right after `if (isPunctuator("<")) { advance();
// int depth = 1; while (depth > 0 && current_.kind != TokenKind::EndOfFile)
// { ... } }`, immediately following the `end` keyword and identifier having
// already been consumed) tracked only `<`/`>` depth, bounded solely by
// `depth == 0` or `EndOfFile`. On a malformed end-label with an unclosed
// `<`, nothing bounded the scan, so it consumed the rest of the file.
//
// MEASURED pre-fix (this commit's parent `9649e92`, with issues #4257 /
// #4341 / #4343's fixes all applied — none of those covers this loop
// because each sibling loop is INSIDE a still-open block and bounds by
// `isKeyword("end")`, the enclosing block's OWN terminator; this loop runs
// AFTER the enclosing type's `end` keyword has already been consumed, so
// `end` is gone from the token stream): exactly ONE diagnostic, "expected
// ';' after 'end'" at EOF (line 10 of a 9-line file). The entire following
// `class CC ... end CC;` (8 lines of source) is silently consumed.
//
// THE FIX: the discard loop now also bounds on `isPunctuator(";")` (the
// end-label's expected terminator; `;` cannot appear inside a well-formed
// generic-parameter spelling — Appendix A.3 `GenericArgs` admits only
// `GenericArg` separated by `,`, balanced `<`/`>`, no statement-level
// punctuation) and on any TopDecl-starting keyword reachable from this
// position (`type` / `extension` / `extern`, the four visibility keywords,
// the four modifier keywords, the five kind keywords `class` / `interface`
// / `record` / `enum` / `choice` — `Parser::parseTopDecl`'s set). None of
// these can appear inside a well-formed generic-parameter end-label
// spelling (none is a valid type-parameter identifier; the GenericArgs
// grammar's terminals are `Identifier`, `,`, `<`, `>`). The pre-fix bug
// suppressed every diagnostic that would otherwise be reported in the
// rest of the file.
//
// Post-fix (this commit), this file produces exactly ONE diagnostic:
// "expected ';' after return statement" INSIDE `class CC4347`'s `Main`
// method body (this file deliberately omits the `;` after `return 0`
// so that a diagnostic naming that exact construct is direct proof the
// parser resynced at the real `;` boundary and went on to parse
// `class CC4347`'s interior — not merely "produced no further
// diagnostics", which alone would be consistent with either recovering
// correctly or silently discarding the class with nothing to say about
// it).
//
// (Same non-discriminating-COUNT note as #4257's/#4341's/#4343's sibling
// tests: pre-fix and post-fix both happen to produce exactly one
// diagnostic LINE here, so only the content-based FileCheck below — not a
// raw `wc -l` — actually distinguishes them. The pre-fix diagnostic
// text is "expected ';' after 'end'" at EOF; the post-fix diagnostic text
// is "expected ';' after return statement" at the real inner location.)
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
class Box4347<T>
  public X: T;
end Box4347<T;
class CC4347
  public static method Main(): int
    return 0
  end Main;
end CC4347;

// CHECK-NOT: expected ';' after 'end'
// CHECK: {{.*}}:{{[0-9]+}}:{{[0-9]+}}: expected ';' after return statement
