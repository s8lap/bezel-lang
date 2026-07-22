// Clean-compile ACCEPT control for the §28.1 (v0.196) `where T: value`
// declaration-site rejection at FUNCTION-TYPE SLOTS and LAMBDA parameters,
// issue #1817. The reject side lives in
// owned_type_requirement_value_constraint_functype_lambda_reject_1817.bzl.
//
// The v0.196 rejection is NARROW: only a bare type parameter constrained
// `where T: value` — the one bound that excludes class and interface
// substitutions outright — loses the `owned` marker at the declaration. Every
// other bare-T `owned` position stays admissible (the marker is erased only
// per-instantiation, at value/runtime-managed substitutions). Over-rejecting
// here would close valid mixed-domain generics, so the same function-type
// slots and lambda parameters must all compile clean when the bound is NOT
// `value`:
//   - `owned` bare-T slot/param under `where T: class`       (class/interface only)
//   - `owned` bare-T slot/param under `where T: IDisposable` (an interface bound)
//   - `owned` bare-T slot/param with NO constraint           (erasure is per-instantiation)
//
// Asserted two ways (as in owned_type_requirement_accept_1797.bzl):
//  1. `%bezelc -c` must EXIT ZERO — a successful compile is the pass.
//  2. The diagnostic stream must contain neither an `error` nor any section
//     citation (`§`) — no false §28.1 reject and no incidental warning.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
class R
  // ---- `where T: class` (class/interface substitutions only) ----
  // function-type PARAMETER slot.
  public method A1<T>(cb: (owned T) -> ()): () where T: class return; end A1;
  // function-type RETURN slot.
  public method A2<T>(cb: () -> owned T): () where T: class return; end A2;
  // explicitly-typed LAMBDA parameter.
  public method A3<T>(): () where T: class
    var f := lambda(owned x: T) => x;
    return;
  end A3;

  // ---- `where T: IDisposable` (an interface bound — not `value`) ----
  public method B1<T>(cb: (owned T) -> ()): () where T: IDisposable return; end B1;
  public method B2<T>(cb: () -> owned T): () where T: IDisposable return; end B2;

  // ---- NO constraint (admitted; erasure is per-instantiation) ----
  public method C1<T>(cb: (owned T) -> ()): () return; end C1;
  public method C2<T>(cb: () -> owned T): () return; end C2;
  public method C3<T>(): ()
    var f := lambda(owned x: T) => x;
    return;
  end C3;
end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
