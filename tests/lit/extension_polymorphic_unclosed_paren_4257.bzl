// fix #4257 (send-back on #4222, §26.3 / Appendix A.4, v0.298).
//
// #4222's `Parser::parseAndDiscardExtensionPolymorphicMember()` (the sibling
// test `extension_polymorphic_diagnostic_cascade_4222.bzl` pins its WELL-
// FORMED-clause behaviour) tracked paren depth bounded only by EndOfFile or
// depth == 0. On a malformed clause with an unclosed `(` — the single most
// likely real typo here — the scan had no bound tied to the enclosing
// extension block's own `end`/`extension` terminator, so it consumed the
// extension's own `end extension;` AND every subsequent top-level
// declaration, all the way to EOF.
//
// MEASURED pre-fix (this commit's parent, `689a145`, i.e. #4222 as
// originally landed): this exact program produced THREE diagnostics — the
// §26.3 rejection, then "expected 'end' to close extension block" and
// "expected ';' after 'end' of extension block", BOTH reported at
// end-of-file — because the scan swallowed `end extension;` (this file's
// line 10) and the entirety of `class CC4257 ...` (lines 11-15) as if they
// were still inside the `polymorphic(` clause.
//
// THE FIX: the depth-tracking scan now also stops on `isKeyword("end")`
// while depth > 0, mirroring `parseExtensionDecl`'s own member-loop
// termination condition (`!isKeyword("end") && ...`) one call frame up.
// This cannot misfire on well-formed input: Appendix A.4 admits, inside the
// parens, only `PolyCase ::= StringLiteral ":" QualifiedName` tokens
// (QualifiedName ::= Identifier ("." Identifier)*) plus "," and the closing
// ")" — never a nested "(" and never `end`, because `end` is in the
// Lexer's reserved-keyword list (Lexer.cpp's `isReservedKeyword`) and so
// always lexes as `TokenKind::Keyword`, never as an `Identifier` a
// `QualifiedName` could contain. So a well-formed clause is structurally
// incapable of reaching depth >= 1 with `end` as the current token; the
// sibling 4222 test staying green (byte-identical diagnostic) is the
// PASS-side proof of that, not a coincidence.
//
// Post-fix, this file produces exactly TWO diagnostics: the same §26.3
// rejection (now pointing at the same line:col as before — the recovery
// bound does not change where the ORIGINAL diagnostic is reported), plus
// "expected ';' after return statement" INSIDE `class CC4257`'s `Main`
// method body (this file deliberately omits the `;` after `return 0` so
// that a diagnostic naming that exact construct is direct proof the parser
// resynced at the real `end extension;` boundary and went on to parse
// `class CC4257`'s interior — not merely "produced no further diagnostics",
// which alone would be consistent with either recovering correctly or
// silently discarding the class with nothing to say about it).

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc -c %s 2>&1 | wc -l | FileCheck %s --check-prefix=COUNT

module M;
class WidgetUnclosed4257
  public X: int;
end WidgetUnclosed4257;
class WidgetUnclosed4257b
  public X: int;
end WidgetUnclosed4257b;
extension WidgetUnclosed4257
  polymorphic("a": WidgetUnclosed4257b;
end extension;
class CC4257
  public static method Main(): int
    return 0
  end Main;
end CC4257;

// CHECK: {{.*}}:{{[0-9]+}}:{{[0-9]+}}: an extension block may not declare a `polymorphic` member registry
// CHECK-SAME: §26.3
// CHECK-NOT: expected 'end' to close extension block
// CHECK: {{.*}}:{{[0-9]+}}:{{[0-9]+}}: expected ';' after return statement

// COUNT: {{^2$}}
