// RUN: %bezelc -c %s 2>&1 | FileCheck %s
//
// §22.6 (v0.307), "`owned` and overload resolution" + the declaration guard:
// ordinary `owned` DOES distinguish overloads, and a pair differing only by it
// is declarable when the differing position's type is a class type — the
// spec's own example is `Append(item: Widget)` beside `Append(owned item: Widget)`.
//
// This file replaces sema_overload__owned_only_difference_diagnoses.bzl, which
// pinned the v0.181 rule ("neither `owned` ... nor `scoped` distinguishes") on
// this exact program. That rule was reversed by the spec, and fix #4618's
// §22.6 declaration guard implements the v0.307 stance; the old test was left
// asserting behaviour the spec no longer describes.
//
// The still-rejected shapes keep their own coverage and are NOT touched here:
//   sema_overload__out_vs_out_owned_clashes.bzl        (`owned` on an `out` parameter)
//   sema_overload__scoped_only_difference_diagnoses.bzl (`scoped`)
//   sema_overload__owned_generic_typeparam_collapse_1873.bzl
//   sema_overload__owned_iface_typeparam_collapse_1873.bzl (guard-failing positions)

module M; class T end T; class C public method F(t: T) _ := 1; end F; public method F(owned t: T) _ := 2; end F; end C;

// The pair is declarable: `T` is a class type, so the §22.6 declaration guard
// passes at the one differing position.
// CHECK-NOT: indistinguishable from another overload
// CHECK-NOT: error:
