// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, record_with_class_field_not_rejected_1272).
//
// CONTROL: a record referencing a CLASS (heap reference) is NOT a value cycle —
// the class field breaks it. Must compile.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Node public V: int; public constructor(v: int) V := v; end constructor; end Node; record Ref(n: Node, tag: int);

// NODIAG-NOT: error
// NODIAG-NOT: §
