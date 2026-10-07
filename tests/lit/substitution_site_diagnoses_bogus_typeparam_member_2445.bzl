// fix #2445 (codegen, LlvmBackend_Layout.cpp fieldAddr, v0.236): the
// structural sibling of #2547's CG8 generic warning. #2547 made the
// CG8 fall-through emit a generic "internal: codegen cannot read
// member 'X' on receiver of type 'T': permissive acceptance (...) but
// no concrete member dispatch exists" — the message names the member
// and the bare type parameter but does NOT name the substituted type
// (the unenumerable, open-ended set of monomorphizations makes a
// per-substitution message impossible at sema time, which is why
// #1078/#2433's retraction left the diagnostic at the call site instead
// of the declaration site). #2445 moves the relevant diagnostic to the
// codegen substitution site (fieldAddr, ~3992), where the active
// genericSubstSemaTy_ binds `T` to its concrete substituted type for
// THIS instantiation (e.g. `T → Pt`). When the loop above resolves the
// member by name on the substituted type's declaration and no member
// matches, #2445 emits a SPECIFIC, REPLACEMENT diagnostic that names
// BOTH the substituted type AND the type parameter: "no member `M` on
// substituted type `Pt` for type parameter `T`" — so the user can
// trace the failure to a specific substituted type's member surface
// (not a generic "on receiver of type `T`") and either correct the
// name or add a `where T: <constraint>` that provides it. The wording
// mirrors sema's #1078 reject (ExprTypecheck_Calls.cpp ~3095) so the
// two diagnostics are recognizably the same family. The CG8 generic
// #2547 warning is unchanged (the sibling control test
// `tests/lit/no_member_unresolvable_field_read_warns_2547.bzl`
// continues to pin that text); #2445 is the *additional* path for the
// bare-T substitution case.
//
// NOTE: fix #5449 (§3.6 rule 2 / §14.2, v0.315) superseded this test's
// expectation. The member access on a bare type parameter now errors at the
// DECLARATION site (not the substitution site) with the
// bare-type-parameter surface diagnostic: "no member `X` on bare type
// parameter `T`: a bare type parameter exposes only the members its
// constraints establish". The `where T: value` constraint does not add
// members to the surface; `value` is not a type/interface bound that
// contributes members via forEachConstraintBound. This test now pins the
// declaration-site diagnostic. The substitution-site #2445 path is
// superseded for the bare-T member-access case.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc -c -o %t.o %s 2>&1 | FileCheck %s

module M; record Pt(X: int, Y: int); class C public static method F<T>(val: T): int where T: value return val.TotallyBogusTypeParamMember; end F; public static method Main(): int var x := F(new Pt(9, 5)); return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `TotallyBogusTypeParamMember` on bare type parameter `T`: a bare type parameter exposes only the members its constraints establish (§3.6 rule 2 / §14.2); add a `where T: <constraint>` that declares `TotallyBogusTypeParamMember`
