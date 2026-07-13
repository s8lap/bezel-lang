// Ported from tests/ownership/self_typed_static_field_1611_test.cpp
//   (BEZEL_TEST self_typed_static_field_1611, mutual_static_field_cycle_compiles).
//
// A MUTUAL static-field cycle (A's static field has type B, B's static field
// has type A) exercises the visited-set across a two-hop chain rather than a
// direct self-reference, confirming the guard generalizes beyond the single-
// decl case. Both records are otherwise value-typed (int-only storage), so
// this must also compile clean.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record A public readonly property V: int; public static readonly Peer: B := new B(1); public constructor(v: int) V := v; end constructor; end A; record B public readonly property W: int; public static readonly Peer: A := new A(2); public constructor(w: int) W := w; end constructor; end B; class C public method Run() var a := new A(1); var b := new B(2); end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
