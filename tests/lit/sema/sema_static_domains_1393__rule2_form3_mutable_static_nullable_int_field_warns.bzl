// §22.12 rule 2 form (iii) (v0.221 ruling / #1393, spec v0.228): "the
// nullable form of either" — a mutable static FIELD of nullable primitive
// type (`int?`) is admitted with the same declaration-site warning as the
// non-nullable form, and the diagnostic spells the type with the `?` intact.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero, but it DOES emit a
// diagnostic — pinned below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static _n: int? := null; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_n` has type `int?` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
