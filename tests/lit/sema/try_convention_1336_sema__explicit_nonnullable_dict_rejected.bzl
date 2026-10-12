// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/try_convention_1336_test.cpp
//   (BEZEL_TEST try_convention_1336_sema, explicit_nonnullable_dict_rejected).
//
// (a) explicit NON-nullable recipient — Dictionary.TryGetValue → REJECT.

module M; class C public static method Main(): int do const d: Dictionary<string, int> := ["a": 1]; var v: int := 0; var ok := d.TryGetValue("a", out v); end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `out` recipient of `TryGetValue` must be nullable: every extraction Try's out parameter is `T?` and the false path assigns `null` (stdlib §1 / §22.3, v0.171 #1336; `out owned var` retired at §A.4, v0.289 L2). Declare the recipient nullable (`T?`) or use an inline `out var` / `out using var` form.
