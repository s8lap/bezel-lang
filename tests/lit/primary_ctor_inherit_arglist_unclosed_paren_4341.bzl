// fix #4341 (§26.3-analogous discard-loop hardening / Appendix A.3, v0.298).
//
// `Parser::parseTypeDeclBody`'s primary-ctor-class inheritance-list parsing
// (the `decl->kind == ast::TypeDeclKind::Class && decl->hasPositionalFields`
// branch, `src/parser/Parser_Decls.cpp`) discards a non-first `InheritItem`'s
// argument list — Appendix A.3 permits an argument list only on the first
// (base-class) item, per the sibling test
// `inherit_header_ctor_args_no_primary_ctor_2455_reject_interface.bzl` for
// the NON-primary-ctor path (`parseInheritance`) — with a paren-depth
// tracking scan bounded only by `depth == 0` or `EndOfFile`.
//
// MEASURED pre-fix (this commit's parent): on an interface item with an
// UNCLOSED `(`, nothing bounded the scan to the enclosing class's own
// `end Foo;` terminator, so it consumed that terminator and the entire
// following `class CC4341 ...` declaration, landing at EOF. This exact
// program produced TWO diagnostics: the real "an argument list is not
// permitted here" rejection, then "expected 'end'" at end-of-file — the
// same defect class `fix #4257` (§26.3/A.4, v0.298) fixed for the
// extension-body `polymorphic(...)` discard loop, in a DIFFERENT discard
// loop in the same file that fix did not cover.
//
// THE FIX: mirror #4257's exact technique — bound the scan by
// `isKeyword("end")` in addition to depth/EOF. This cannot misfire on
// well-formed input: Appendix A's `ArgList` grammar admits only expression
// tokens inside the parens, and `end` is a reserved keyword
// (`Lexer.cpp`'s `isReservedKeyword`), so it can never appear inside a
// well-formed argument list.
//
// Post-fix, this file produces exactly TWO diagnostics: the same "an
// argument list is not permitted here" rejection (same location as
// before — the recovery bound does not change where the ORIGINAL
// diagnostic is reported), plus "expected ';' after return statement"
// INSIDE `class CC4341`'s `Main` method body (this file deliberately
// omits the `;` after `return 0` so that a diagnostic naming that exact
// construct is direct proof the parser resynced at the real `end
// WidgetInheritArgs4341;` boundary and went on to parse `class CC4341`'s
// interior — not merely "produced no further diagnostics", which alone
// would be consistent with either recovering correctly or silently
// discarding the class with nothing to say about it).

// Deliberately ONE RUN line, not the `wc -l`/COUNT pair the sibling #4257
// test uses: for THIS probe shape, pre-fix and post-fix both produce
// exactly two diagnostic LINES (pre-fix: the rejection + "expected 'end'"
// at real EOF; post-fix: the rejection + the real return-statement defect)
// — a coincidental tie that makes a raw line-count check non-discriminating
// here. The content-based FileCheck below (CHECK-NOT the swallow's
// "expected 'end'", CHECK the real resync-site diagnostic) is what
// actually distinguishes the two, and is fully discriminating on its own.
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
class BaseInheritArgs4341
  public X: int;
end BaseInheritArgs4341;
interface IBarInheritArgs4341
end IBarInheritArgs4341;
class WidgetInheritArgs4341(x: int): BaseInheritArgs4341(x), IBarInheritArgs4341(unclosed
  public Y: int;
end WidgetInheritArgs4341;
class CC4341
  public static method Main(): int
    return 0
  end Main;
end CC4341;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: expected 'end'
// CHECK: {{.*}}:{{[0-9]+}}:{{[0-9]+}}: an argument list is not permitted here
// CHECK-SAME: permitted only on the base class
// CHECK-SAME: not on an interface
// CHECK-NOT: expected 'end'
// CHECK: {{.*}}:{{[0-9]+}}:{{[0-9]+}}: expected ';' after return statement
// #7065: and the same needle again, AFTER the last positive match, so the
// prohibition covers the tail of the output too — a negative directive
// scans only the region between the surrounding matches, and this one was
// bounded by the next match.
// CHECK-NOT: expected 'end'

// COUNT: {{^2$}}
