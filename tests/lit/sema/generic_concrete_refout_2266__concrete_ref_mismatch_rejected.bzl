// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §22.3 "ref and out argument locations" + §14.5 (spec v0.228) / #2266.
//
// A `ref` argument aliases the caller's storage location directly (§22.3: "A
// `ref` argument naming a mutable local or writable field is a true alias"), so
// no conversion may be admitted at the position (§14.5: "a `ref` or `out`
// parameter position unifies exactly: the parameter aliases the caller's
// location, so no conversion is admitted at the binding or later at the
// applicability check").
//
// #1756's §22.3 check skipped EVERY generic method wholesale and #1686's §14.5
// check bails unless some ref/out parameter's declared type mentions a METHOD
// type parameter — so a generic method with a CONCRETELY-typed ref parameter
// (`ref slot: int` on `Store<T>`) fell between the two and was checked by
// neither. Codegen then passed the caller's raw `string` address for an `int`
// referent (LlvmBackend_Calls2 `lvalueAddr` — no coercion, no temp copy), so the
// callee's `slot := 1` wrote an `int` through a `string*`: type confusion.
//
// WITHOUT the fix this program compiles clean (exit 0), so `%not` fails.

module M; static class U public static method Store<T>(v: T, ref slot: int) slot := 1; end Store; end U; class C public static method Main(): int var s: string := "hi"; U.Store<int>(5, ref s); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `ref` argument for parameter `slot` to call to `Store` does not match the parameter's referent type
// CHECK-SAME: aliases the caller's location (§22.3)
// CHECK-SAME: no implicit conversion is admitted (§14.5)
