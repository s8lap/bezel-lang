// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5312 / stdlib §1 (v0.313) + spec §22.4 / §7.11: after the `if`, the
// call may have returned false (or not run), so the recipient of
// `Dictionary.TryGetValue(…, out var v)` is still `int?`; `v + 1` is rejected.
//
// REJECT: arithmetic on the recipient after the controlled `if`.

module M; class C public static method Main(): int var d := new Dictionary<string, int>(); if d.TryGetValue("a", out var v) then end; Console.WriteLine(v + 1); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` is not defined on a nullable value type
