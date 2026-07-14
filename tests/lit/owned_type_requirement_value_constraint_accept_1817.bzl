// Clean-compile ACCEPT control for the §28.1 (v0.196) `where T: value`
// declaration-site rejection, issue #1817. The reject side lives in
// owned_type_requirement_value_constraint_reject_1817.bzl.
//
// The v0.196 rejection is NARROW: only a bare type parameter constrained
// `where T: value` — the one bound that excludes class and interface
// substitutions outright — loses the `owned` marker at the declaration. Every
// other bare-T `owned` position stays admissible (the marker is erased only
// per-instantiation, at value/runtime-managed substitutions). Over-rejecting
// here would close valid mixed-domain generics, so these must all compile clean:
//   - `owned` bare-T under `where T: class`     (class/interface substitutions only)
//   - `owned` bare-T under `where T: IDisposable` (an interface bound — not `value`)
//   - `owned` bare-T with NO constraint          (admitted; erasure is per-instantiation)
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
  // owned bare-T under `where T: class` — transferred out via the owned return.
  public method A1<T>(owned x: T): owned T where T: class return x; end A1;
  // owned bare-T under `where T: IDisposable` — an interface bound, not `value`.
  public method A2<T>(owned x: T): owned T where T: IDisposable return x; end A2;
  // owned bare-T with NO constraint — admitted (erasure is per-instantiation).
  public method A3<T>(owned x: T): owned T return x; end A3;
end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
