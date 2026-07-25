// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Spec §22.3 (v0.226) / #1756: the `out` half of the same no-conversion
// rule the `ref`-mismatch neighbor pins. A pre-existing `out` recipient of
// type `long` passed to an `out x: int` parameter must be REJECTED on the
// same "aliases the caller's location, so the type must match exactly"
// principle — the callee writes through the alias, so a width mismatch
// would store a partial / mismatched-width value. (The inline-declaration
// family `out var` / `out using var` is unaffected: its type is inferred
// FROM the parameter (§22.4) and matches by construction.)
module M;
class C
  public static method Fill(out x: int) x := 42; end Fill;
  public static method Main(): int
    var n: long := 0;
    C.Fill(out n);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `out` argument for parameter `x` to {{.*}} does not match the parameter's referent type
// CHECK-SAME: a `ref`/`out` argument aliases the caller's location, so the type must match exactly and no implicit conversion is admitted
// CHECK-SAME: (§22.3)
