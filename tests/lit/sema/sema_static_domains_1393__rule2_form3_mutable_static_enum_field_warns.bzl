// §22.12 rule 2 form (iii) (v0.221 ruling / #1393, spec v0.228): a mutable
// static FIELD of enum type is admitted with a declaration-site warning
// (the same posture as the primitive leg — an enum is a value type with no
// reference content, so the risk is stale/torn values, never memory
// unsafety).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero, but it DOES emit a
// diagnostic — pinned below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; enum Color Red, Green, Blue; end Color; class App public static _c: Color := Color.Red; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_c` has type `Color` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
