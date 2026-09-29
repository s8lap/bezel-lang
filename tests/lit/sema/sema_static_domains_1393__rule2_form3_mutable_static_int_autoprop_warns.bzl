// §22.12 rule 2 form (iii) (v0.221 ruling / #1393, spec v0.228): the
// "fields only" qualifier in rule 2's form (iii) text scopes ONLY the
// plain-inline-array alternative ("or — fields only — a plain inline
// array...") — the primitive/enum leg (and its nullable form) is admitted
// for auto-properties exactly like fields, with the same declaration-site
// warning. A naive "forms (iii)/(iv) are fields only" reading would
// wrongly reject this; it must WARN, not error.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero, but it DOES emit a
// diagnostic — pinned below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static property Count: int := 0; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static property `Count` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
