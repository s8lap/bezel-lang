// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for issue #1827 (spec §32.5, v0.226).
//
// §32.5 "Nullable value types are rejected": `int?`, `bool?`, `IntPtr?`,
// `Ptr<T>?`, and nullable record forms have no C representation. Only the
// pointer-carrying boundary forms (`CString?`/`WString?`/`RawBytes?`) admit
// `?`. An inline array `[N]T` is a value type — so a nullable inline array
// and an inline array of a nullable value-typed element fall in the same
// class and must be rejected.
//
// Per the parser (Parser_Decls.cpp `parseType()`, `[4]int?`): the inner
// `parseType()` absorbs a trailing `?` ON THE ELEMENT, so the single
// spelling `[N]int?` is the nullable-element case (the array-itself
// nullable form is admitted by the parser and flagged by sema in its own
// check, but is not commonly written). The sema fix (TypeResolution_Checks.cpp,
// §32.5) rejects BOTH cases — `t.nullable` AND `elem.nullable` — with the
// same `[N]int?` diagnostic; either-or is enough.
//
// Mirror of the Named case's "if (t.nullable) return false;" (`int?`,
// `IntPtr?` — #1715). The InlineArray branch previously ignored both
// `t.nullable` and `elem.nullable`, so the source's `t.nullable = true`
// or `elem.nullable = true` slipped through with the wrong-positive
// "primitive element" branch.

module M;
extern "c"
  public method takes_nullable_element(x: [4]int?): int;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `x` has type `[N]int?`, which is a nullable value type — nullable value types are rejected at the FFI boundary
// CHECK-SAME: (§32.5)
