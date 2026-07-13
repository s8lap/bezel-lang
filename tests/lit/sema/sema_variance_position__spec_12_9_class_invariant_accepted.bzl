// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_class_invariant_accepted).
//
// A class with an `out`-annotated type param is rejected semantically
// (issue #1559, Group F below) — classes are always invariant. Here we only
// confirm an ordinary invariant class generic compiles fine.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box<T> private _value: T?; public method Get(): T? => _value; end Box;

// NODIAG-NOT: error
// NODIAG-NOT: §
