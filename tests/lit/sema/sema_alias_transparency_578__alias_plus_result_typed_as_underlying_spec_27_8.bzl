// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, alias_plus_result_typed_as_underlying_spec_27_8).
//
// 6a §27.8 — Meters + Meters result is typed as double (the alias's underlying
// type). The inferred result local must carry a Primitive/Double declType.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Meters := double; class C public method M() var a: Meters := 5.0; var b: Meters := 3.0; var sum := a + b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
