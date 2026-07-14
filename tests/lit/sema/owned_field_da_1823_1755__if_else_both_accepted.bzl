// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// spec §7.9 (Appendix C.1), v0.196 — non-nullable field definite assignment at
// constructor normal exit. ACCEPT control for #1823.
//
// `_box` is assigned on BOTH branches of the `if`, so it is definitely assigned
// on every normal-exit path — ACCEPTED.

module M;
class Box public value: int; public constructor() value := 7; end constructor; end Box;
sealed class W
  private owned _box: Box;
  public constructor(b: bool)
    if b then _box := new Box(); else _box := new Box(); end;
  end constructor;
  public method V(): int return _box.value; end V;
end W;

// NODIAG-NOT: error
// NODIAG-NOT: §
