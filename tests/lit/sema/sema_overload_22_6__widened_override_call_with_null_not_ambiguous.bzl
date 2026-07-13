// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, widened_override_call_with_null_not_ambiguous).
//
// §12.1 (v0.181, #1560) / §22.6 (#1090) regression: calling a widened
// override through the declaring class's OWN static type must resolve to
// exactly one candidate. Before this fix, collectHierarchyOverloads' same-
// class dedup ("shadows") required an EXACT parameter-type match, so a
// widened override (`a: string?`) did NOT shadow its base's non-widened
// declaration (`a: string`) — the base declaration then lingered in the
// candidate set alongside the override, and a `null` argument (whose
// default, un-target-typed type is `Object?`) classified as
// ArgMatch::NotApplicable against BOTH same-arity candidates under generic
// §22.6 classification, producing a spurious "no overload is applicable"
// error instead of resolving to the (sole intended) widened override.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual method F(a: string): string => a; end Base; class D : Base public override method F(a: string?): string => ""; public method Use() _ := this.F(null); end Use; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
