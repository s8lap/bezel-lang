// Ported from tests/ownership/self_typed_static_field_1611_test.cpp
//   (BEZEL_TEST self_typed_static_field_1611, origin_style_static_field_compiles).
//
// The issue's exact repro (§9.2's own `Origin`-style canonical example): a
// body record whose only body-record storage is two auto-properties (parsed
// as PropertyDecl, never walked by isBorrowBearingRecord's FieldDecl-only
// member loop) plus a `static readonly` field of the record's OWN type. Prior
// to the fix, checking this record's borrow-bearing status (triggered here by
// `handleLocalDecl`'s borrow-lifetime check on `var a := new Point(1, 2);`)
// recursed into the same TypeDecl without bound and stack-overflowed the
// compiler. Must now compile with NO diagnostics.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point public readonly property X: int; public readonly property Y: int; public static readonly Origin: Point := new Point(0, 0); public constructor(x: int, y: int) X := x; Y := y; end constructor; end Point; class C public method Run() var a := new Point(1, 2); end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
