// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3856 (§22.12, v0.294): a class-level `const` with NO written type
// annotation is classified against §22.12's "primitive value type or
// `string`" requirement by its INITIALIZER's type, exactly as an explicitly
// `: Priority`-typed sibling already is. Before this fix, checkTypeConsts
// (TypeResolution_Checks.cpp) skipped every const with no declared type
// ("inferred — deferred") and no later pass picked up the deferral, so
// `public const J := Priority.Low;` compiled silently even though the
// spelled-out form of the exact same const is rejected.

module M; enum Priority Low, High; end Priority; class C public const J := Priority.Low; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `J` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
