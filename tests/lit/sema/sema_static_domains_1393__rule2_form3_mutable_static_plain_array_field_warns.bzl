// §22.12 rule 2 form (iii) (v0.221 ruling / #1393, spec v0.228): a mutable
// static FIELD of plain (non-`owned`) inline-array type, `[N]T`, whose
// element type is itself form-(iii)-eligible (here `int`), is admitted with
// a declaration-site warning.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero, but it DOES emit a
// diagnostic — pinned below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static _buf: [3]int := [1, 2, 3]; public static method Main(): int => _buf[0]; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_buf` has type `[3]int` — a plain inline array of primitive/enum element type is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
