// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §22.3 "ref and out argument locations" + §14.5 (spec v0.228) / #2266 — the
// `out` half of the same hole. §22.3 gives `out` the same argument-location
// rules as `ref` and §14.5 names both modifiers in its exactness bullet, so a
// pre-declared `out` recipient on a generic method with a CONCRETELY-typed out
// parameter must be rejected on a type mismatch too.
//
// WITHOUT the fix this program compiles clean (exit 0), so `%not` fails.

module M; static class U public static method Fill<T>(v: T, out slot: int) slot := 7; end Fill; end U; class C public static method Main(): int var s: long := 1; U.Fill<int>(5, out s); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `out` argument for parameter `slot` to call to `Fill` does not match the parameter's referent type
// CHECK-SAME: aliases the caller's location (§22.3)
// CHECK-SAME: no implicit conversion is admitted (§14.5)
