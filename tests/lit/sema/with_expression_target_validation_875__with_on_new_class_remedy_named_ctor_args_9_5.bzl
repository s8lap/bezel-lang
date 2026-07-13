// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, with_on_new_class_remedy_named_ctor_args_9_5).
//
// ACCEPT (§22.5 remedy): the diagnostic's first remedy — named/optional
// constructor arguments — actually compiles in place of the rejected form.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public property V: int; public constructor(v: int) V := v; end constructor; destructor end destructor; end Box; class C public static method Main(): int using var b := new Box(v := 42); return b.V; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
