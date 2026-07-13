// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, readonlyarrayspan_documented_surface_allowed_4_15).
//
// (b) ReadOnlyArraySpan<T> documented surface still compiles without error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Test() using var arr := new Array<int>(); arr.Add(1); arr.Add(2); var sp := arr.AsReadOnlySpan(); var _len := sp.Length; var _emp := sp.IsEmpty; var _idx := sp.IndexOf(1); var _cnt := sp.Contains(1); var _sl := sp.Slice(0, 1); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
