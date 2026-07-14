// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §28.1 "Type requirement" (bare-type-parameter arm revised v0.196),
// issue #1817. The v0.196 revision replaces v0.193's instantiation-site
// rejection with per-instantiation ERASURE of an `owned` marker on a bare type
// parameter — but keeps one declaration-site rejection: "A declaration whose
// constraints exclude class and interface substitutions outright
// (`where T: value`) rejects the marker at the declaration — it could never be
// more than redundant, the concrete-type reasoning above applied at the only
// point it is decidable." A `value`-constrained parameter can never be a class
// or interface, so `owned` on it has nothing to own.
//
// This is the reject side; the clean-compile accept controls (`where T: class`,
// `where T: IDisposable`, and UNCONSTRAINED bare-T `owned`, none of which this
// arm may false-reject) live in owned_type_requirement_value_constraint_accept_1817.bzl.
//
// Before the fix each case below compiled clean (the bare-T branch of
// checkOwnedDeclType admitted every type parameter unconditionally), so this
// reject test finds no §28.1 diagnostic and fails without the fix; it passes
// with it. The param and method-return diagnostics are pinned to their exact
// declaration line with `[[@LINE-1]]`; the operator declaration carries no
// source span (it lands at :0:0:), so its diagnostic is matched by message text.
module M;
class R
  // owned PARAMETER whose type is a bare parameter constrained `where T: value`.
  public method P1<T>(owned x: T): () where T: value return; end P1;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a parameter of type a type parameter that can never be a class or interface (`where T: value`)
  // CHECK-SAME: (§28.1)

  // owned bare-type-parameter RETURN under the same `where T: value` bound.
  public method P2<T>(): owned T where T: value return default(T); end P2;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a return type of type a type parameter that can never be a class or interface (`where T: value`)
  // CHECK-SAME: (§28.1)
end R;

// An operator's `owned` return (the only owned-admitting position on an
// operator, §25.2) whose type is the enclosing type's `value`-constrained
// parameter — the bound is read from the enclosing type's constraints, not the
// operator's (operators carry none).
class Box<T> where T: value
  public static method operator +(a: Box<T>, b: Box<T>): owned T => default(T);
  // CHECK: `owned` is not admissible on a return type of type a type parameter that can never be a class or interface (`where T: value`) (§28.1)
end Box;
