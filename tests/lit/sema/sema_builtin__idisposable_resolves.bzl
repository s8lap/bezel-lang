// Ported from tests/sema/builtin_types_test.cpp
//   (BEZEL_TEST sema_builtin, idisposable_resolves).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// fix #4326 (§28.2, v0.298): `_x` is now `IDisposable?` (nullable), not bare
// `IDisposable`. #4326 registered `IDisposable` in
// `common::kHeapPointerReferenceBuiltins`, so it is now correctly recognized
// as the heap-pointer reference type it always was — and, like every OTHER
// class/interface-typed field in the language, a NON-NULLABLE field of that
// type now correctly demands an initializer or an assigning constructor
// (§7.9's post-#4025 strict-storage rule, LANE_RULES §22) instead of
// silently compiling with no initializer at all. That is a NEW, CORRECT
// rejection this fix's representation correction exposes, not something
// #4326 broke: the old bare-`IDisposable` field pinned the exact
// uninitialized-reference-field hazard §7.9 exists to catch for every other
// class-typed field. `?` keeps this test asserting the thing it always
// meant to assert — that `IDisposable` RESOLVES as a type name — without
// depending on §7.9's unrelated field-initialization rule.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _x: IDisposable?; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
