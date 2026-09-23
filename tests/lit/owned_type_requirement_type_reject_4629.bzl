// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §28.9 "Runtime-managed types" (v0.300, #4393 ruling) + §28.1 "Type
// requirement" (v0.193/v0.196), issue #4629. §28.9 names `Type` — the
// process-interned type-identity token — runtime-managed: "no `Type`
// expression is an owned production, none lends a borrow, and no holder
// carries an obligation for one; `owned` over `Type` is inadmissible
// (§28.1, §28.5)." §28.1's own type-requirement paragraph makes this
// explicit: `owned` requires "a class type other than `Type`
// ([§28.9]...), an interface type, or a bare type parameter", and its
// clause 2 lists `Type` by name among the rejected types alongside a
// primitive, `string`, a function type, an ordinary record, an enum, a
// non-owning inline array, and the unit type.
//
// Before the #4629 fix, `Resolver::checkOwnedDeclType`'s named-but-
// declaration=null arm (TypeResolution_Names.cpp — the arm that handles
// builtins whose Named TypeRef intentionally carries `declaration=null`,
// which is how `Type` reaches this site) had no `Type` case, so every
// method below compiled clean: `bezelc -c` exited 0 with a §28.3 auto-drop
// warning, and a body access on the owned `Type` parameter (e.g. `.Name`)
// reached codegen and crashed with an internal "failed to lower to a
// value" error (§3079) instead of failing cleanly at sema. That crash
// diagnostic carries its own real location, so this test's oracle is the
// SPECIFIC §28.9/§28.1 message text below — never
// `failedAtCompileTimeWithRealLocation`, which would report true on both
// sides of the fix (the §3079 backstop already has a real location).
//
// Each declaration-position diagnostic is pinned with `[[@LINE-1]]`, as in
// owned_type_requirement_reject_1797.bzl; the function-type-slot
// diagnostics carry no precise source span (as in
// owned_type_requirement_functype_lambda_reject_1797.bzl) and are matched
// by message only. FileCheck matches these in source order (P1, P2, P3,
// F1, F2, G).
//
// Covers every position of §28.1's list that funnels through
// `checkOwnedDeclType` (method parameter, `out` parameter, return type,
// function-type parameter/return slot, lambda parameter). The operator-
// return-type position is omitted: it reaches the identical code path and
// diagnostic text as the method-return-type case below (both call
// `checkOwnedDeclType(..., "a return type")`). The choice-payload position
// is not on this check's call graph at all and is out of scope for this
// fix.
module M;
class R
  // `owned` on a Type-typed PARAMETER.
  public method P1(owned t: Type): () return; end P1;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a parameter of type the runtime `Type` token (a runtime-managed reference type, §28.9)
  // CHECK-SAME: (§28.1)

  // `out owned` on a Type-typed OUT PARAMETER.
  public method P2(out owned t: Type): () t := typeof(int); return; end P2;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on an `out` parameter of type the runtime `Type` token (a runtime-managed reference type, §28.9)
  // CHECK-SAME: (§28.1)

  // `owned` Type-typed RETURN.
  public method P3(): owned Type return typeof(int); end P3;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a return type of type the runtime `Type` token (a runtime-managed reference type, §28.9)
  // CHECK-SAME: (§28.1)

  // `owned` on a function-type PARAMETER slot at `Type`.
  public method F1(cb: (owned Type) -> ()): () return; end F1;
  // CHECK: `owned` is not admissible on a function-type parameter slot of type the runtime `Type` token (a runtime-managed reference type, §28.9)
  // CHECK-SAME: (§28.1)

  // `owned` on a function-type RETURN slot at `Type`.
  public method F2(cb: () -> owned Type): () return; end F2;
  // CHECK: `owned` is not admissible on a function-type return slot of type the runtime `Type` token (a runtime-managed reference type, §28.9)
  // CHECK-SAME: (§28.1)

  // `owned` on a LAMBDA parameter with an explicit `Type` annotation.
  public method G(): ()
    var f := lambda(owned x: Type) => x;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a lambda parameter of type the runtime `Type` token (a runtime-managed reference type, §28.9)
    // CHECK-SAME: (§28.1)
    return;
  end G;
end R;
