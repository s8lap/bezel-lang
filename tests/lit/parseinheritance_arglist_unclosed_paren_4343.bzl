// fix #4343 (third instance of the discard-loop-swallow class §26.3/A.4
// `fix #4257` [v0.298] hardened, and #4341 [Appendix A.3, v0.298] hardened
// a second copy of; Appendix A.3, v0.298).
//
// `Parser::parseInheritance` — the function `parseTypeDeclBody` calls for
// record/interface decls and classes with NO primary constructor (see the
// sibling `primary_ctor_inherit_arglist_unclosed_paren_4341.bzl`, which
// pins the SEPARATE copy of this same loop in the primary-ctor-class
// branch) — discards a non-first `InheritItem`'s argument list with a
// paren-depth-tracking scan bounded only by `depth == 0` or `EndOfFile`.
//
// MEASURED pre-fix (this commit's parent): on an interface item with an
// UNCLOSED `(`, nothing bounded the scan to the enclosing type's own
// `end Foo;` terminator, so it consumed that terminator and the entire
// following `class CC4343 ...` declaration, landing at EOF. This exact
// program produced TWO diagnostics: the real "an argument list is not
// permitted here" rejection, then "expected 'end'" at end-of-file.
//
// THE FIX: mirror #4257's/#4341's exact technique — bound the scan by
// `isKeyword("end")` in addition to depth/EOF. Cannot misfire on
// well-formed input: Appendix A's `ArgList` grammar admits only
// expression tokens inside the parens, and `end` is a reserved keyword
// (`Lexer.cpp`'s `isReservedKeyword`), so it can never appear inside a
// well-formed argument list.
//
// Post-fix, this file produces exactly TWO diagnostics: the same
// rejection (same location as before), plus "expected ';' after return
// statement" INSIDE `class CC4343`'s `Main` method body (deliberately
// missing `;` — direct proof the parser resynced at the real `end
// WidgetNoPrimaryCtor4343;` boundary and went on to parse `class
// CC4343`'s interior, not merely "produced no further diagnostics").
//
// (Same non-discriminating-COUNT note as #4341's sibling test: pre-fix and
// post-fix both happen to produce exactly two diagnostic LINES here, so
// only the content-based FileCheck below — not a raw `wc -l` — actually
// distinguishes them.)
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
interface IBaseNoPrimaryCtor4343
end IBaseNoPrimaryCtor4343;
interface IBarNoPrimaryCtor4343
end IBarNoPrimaryCtor4343;
class WidgetNoPrimaryCtor4343: IBaseNoPrimaryCtor4343, IBarNoPrimaryCtor4343(unclosed
  public Y: int;
end WidgetNoPrimaryCtor4343;
class CC4343
  public static method Main(): int
    return 0
  end Main;
end CC4343;

// CHECK: {{.*}}:{{[0-9]+}}:{{[0-9]+}}: an argument list is not permitted here
// CHECK-SAME: permitted only on the base class
// CHECK-SAME: not on an interface
// CHECK-NOT: expected 'end'
// CHECK: {{.*}}:{{[0-9]+}}:{{[0-9]+}}: expected ';' after return statement
