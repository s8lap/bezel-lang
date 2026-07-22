// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §28.1 "Type requirement" (bare-type-parameter arm revised v0.196),
// issue #1817 — the FUNCTION-TYPE-SLOT and LAMBDA-PARAMETER arms of the
// `where T: value` declaration-site rejection. The v0.196 revision replaces
// v0.193's instantiation-site rejection with per-instantiation ERASURE of an
// `owned` marker on a bare type parameter, but keeps one declaration-site
// rejection: "A declaration whose constraints exclude class and interface
// substitutions outright (`where T: value`) rejects the marker at the
// declaration — it could never be more than redundant." A `value`-constrained
// parameter can never be a class or interface, so `owned` on it has nothing to
// own — and that verdict is independent of the POSITION the marker sits in.
//
// R1 (landed) tightened checkOwnedDeclType's bare-T branch to consult the
// enclosing method/operator + type constraints; this test locks in that the
// same reject reaches the three positions that route through the SAME
// checkOwnedDeclType helper during signature/body type-resolution:
//   - a function-type PARAMETER slot   `(owned T) -> ()`
//   - a function-type RETURN slot      `() -> owned T`
//   - an explicitly-typed LAMBDA param `(owned x: T) => ...`
// Before R1 the bare-T branch admitted every type parameter unconditionally, so
// all three compiled clean and this reject test finds no §28.1 diagnostic — it
// fails without the fix and passes with it. The clean-compile accept controls
// (`where T: class`, `where T: IDisposable`, and UNCONSTRAINED bare-T at the
// same positions, none of which this arm may false-reject) live in
// owned_type_requirement_value_constraint_functype_lambda_accept_1817.bzl.
//
// The function-type-slot diagnostics carry no precise source location (the
// function-type AST node populates no span, so they land at :0:0:) and are
// matched by message; the lambda-parameter diagnostic is pinned to its
// expression line. FileCheck matches these in source order (F1, F2, then G).
module M;
class R
  // `owned` on a function-type PARAMETER slot whose type is a bare parameter
  // constrained `where T: value`.
  public method F1<T>(cb: (owned T) -> ()): () where T: value return; end F1;
  // CHECK: `owned` is not admissible on a function-type parameter slot of type a type parameter that can never be a class or interface (`where T: value`)
  // CHECK-SAME: (§28.1)

  // `owned` on a function-type RETURN slot at the same `where T: value` bound.
  public method F2<T>(cb: () -> owned T): () where T: value return; end F2;
  // CHECK: `owned` is not admissible on a function-type return slot of type a type parameter that can never be a class or interface (`where T: value`)
  // CHECK-SAME: (§28.1)

  // `owned` on an explicitly-typed LAMBDA parameter whose type is the enclosing
  // method's `where T: value` parameter.
  public method G<T>(): () where T: value
    var f := lambda(owned x: T) => x;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a lambda parameter of type a type parameter that can never be a class or interface (`where T: value`)
    // CHECK-SAME: (§28.1)
    return;
  end G;
end R;
