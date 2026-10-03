// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_5328__lib.bzl
// RUN: %not %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck %s
//
// Issue #5328 / spec §15.4 (v0.313) — CONTROL ROW: an INLINE-ARRAY `[3]int`
// formal, imported, with a matching 3-element literal.
//
// §15.4's first bullet on the closed target list is "Inline arrays `[N]T`,
// `[N]T?`, `[N owned]T`, `[N owned]T?` (the literal must have exactly N
// elements)", and the section's own worked example is `ProcessBuffer([1, 2,
// 3]); // type from parameter`. An inline-array formal is a first-class
// §15.4 target, so sema MUST NOT reject `Inl([1, 2, 3])` at `Inl(a: [3]int)`.
//
// This row pins that boundary, and it is the row that caught the over-reach:
// the first form of this lane's fix extended the local arm's whole #3031
// `isCollectionLiteralAdmissibleTarget` gate across the module boundary, and
// that predicate identifies a builtin head through `t->astNode` — null on an
// imported formal, whose TypeRef carries its name on `importedName` (the
// #2648 point-1 blind spot this issue's item 4 also names). MEASURED: it
// false-rejected this very program with "cannot target the type `[3]int`",
// a §15.4-admitted target. The shipped fix gates on
// `isCollectionLiteralInterfaceTarget` only, which matches by simple name
// through `importedName` and is therefore correct across the boundary.
//
// The `%not` + `CHECK` combination is deliberate. This program is STILL
// rejected — by codegen's `lowerCallDispatch` imported-argument arm, whose
// message ("cannot target the imported parameter type here") contradicts
// itself by listing an inline array among the admissible targets. That
// residual false-reject is issue #5328's item 2 and belongs to the SEPARATE
// CODEGEN arm (`buildOrderedArgs`' inline-array path is local-only); this
// lane is the sema side and does not touch it. So the row asserts exactly
// the sema-side fact and refuses to assert the codegen arm's outcome:
// the diagnostic still present is the CODEGEN one, and the SEMANTIC
// "cannot target" rejections are absent. A future codegen lane that admits
// this correctly will make the `%not` fail, and that is the intended signal
// to revisit this row — not a silent drift.
module App5328Inl;

import Lib5328;

public class Program
  public static method Main(): int
    Console.WriteLine(Lib5328.Ops.Inl([1, 2, 3]));
    return 0;
  end Main;
end Program;

// The residual rejection is codegen's, not sema's.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the imported parameter type here
// …and sema stayed silent on the inline-array target §15.4 admits.
// CHECK-NOT: cannot target the interface type
// CHECK-NOT: cannot target the type
// CHECK-NOT: cannot target the inline array
// CHECK-NOT: internal:
