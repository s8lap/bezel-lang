// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #3298 (§17.5 / §16.6 / §7.11, spec v0.286): "`in` states no nullability rule
// of its own: `x in c` *is* the call `c.Contains(x)` … Where the selected
// `Contains` declares a **non-nullable** parameter, a nullable left operand is
// a **compile error** … This covers **every range test**: stdlib §9.8 declares
// `Contains(value: T)` over the non-nullable `T` of §16.3, and range
// membership is an ordering test, which admits no absent participant — so
// `n in 1..5` over an `int?` is rejected" (§17.5).
//
// Both #3298 and proposal 0114 asserted this was ALREADY a compile error.
// Measured on the pre-fix compiler it was not: it reached codegen and died in
// IR verification (`icmp sge {i8,i32}, i32`). §16.6's parity claim — that
// `n in 1..5` and `case n of 1..5` agree — held on neither side.

// The scrutinee is a PARAMETER, not `var n: int? := 5;` — an initializer of a
// non-null value narrows the local (§7.4), and §7.11's static-type reading then
// makes it a non-nullable operand that `Contains` legitimately admits. A
// parameter is un-narrowed, which is the shape §17.5's rule is about.

module M; class C public static method Probe(n: int?): bool return n in 1..5; end Probe; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the left operand of `in` over a range is nullable — a range's `Contains` takes the non-nullable element type (stdlib §9.8) and range membership is an ordering test, which admits no absent participant (§16.3, §16.6, §7.11); narrow first
