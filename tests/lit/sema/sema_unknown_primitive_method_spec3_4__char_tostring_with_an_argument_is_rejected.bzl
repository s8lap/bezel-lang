// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, char_tostring_is_accepted).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// §3.4 / stdlib §9.5 (v0.345): char declares `public pure method ToString():
// string;`. Stdlib §2.18 (v0.345) lists `char` among the intrinsically
// formattable types with an EMPTY vocabulary — "a format spec on these is a
// compile error" — so the `ToString(format: string)` FORM is declared for
// `char` (fix #7333 / #7336 put the row back into the built-in parameter
// table) and a `char` ARGUMENT reaches no candidate: §5.1 converts no
// `char` to a `string` implicitly. Language spec §22.6 (v0.345) makes that
// "a compile error with no candidates cited".
//
// THE FLIP: this file used to be a CLEAN-COMPILE ACCEPT control — it pinned
// `char.ToString('z')` as compiling with no diagnostics, on the old §3.4
// "primitive method" surface. The standard library declares no such
// overload, so the call is a compile error; campaign 2's built-in call
// admission gate (the ONE parameter table + the ONE resolver) now refuses it
// with that one diagnostic. The row keeps the shape it pins and its
// expectation is now the refusal.
//
// THE DIAGNOSTIC TEXT (fix-forward): the refusal names the DECLARED form the
// argument misses — the `format` parameter of the `ToString(format: string)`
// form — not an arity mismatch. The arity wording this file pinned before
// the `ToString(format: string)` row went back into the table is GONE, and
// that text change is named in the commit body.

module M; class C public static method Main(): int var s := char.ToString('z'); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no matching overload of `char.ToString`
// CHECK-SAME: stdlib §9.5 declares `ToString(format: string): string`
// CHECK-SAME: so the `format` argument must be a `string`
// CHECK-SAME: no candidates cited (§22.6)
