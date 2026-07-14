// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §7.9 (Appendix C.1), v0.196 — non-nullable field definite assignment at
// constructor normal exit. Regression for #1823 (and #1755's REJECT direction).
//
// `_name` is assigned in the `1` arm but the `_` catch-all is an empty `do end`
// that leaves it unset — a live normal-exit path with `_name` null. Adding a
// `CaseStmt` branch to the old any-path collector (the naive #1755 fix) would
// wrongly ACCEPT this; the all-paths arm-intersection REJECTS it.

module M;
sealed class W
  private _name: string;
  public constructor(n: int)
    case n of
      1 => do _name := "a"; end,
      _ => do end
    end;
  end constructor;
  public method N(): string return _name; end N;
end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: constructor does not initialize non-nullable field `_name`
// CHECK-SAME: (§7.9
