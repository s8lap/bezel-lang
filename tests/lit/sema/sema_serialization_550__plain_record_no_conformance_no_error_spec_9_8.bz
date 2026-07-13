// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, plain_record_no_conformance_no_error_spec_9_8).
//
// §9.8 suppression: a record that does NOT declare ISerializable or
// IDeserializable gets no synthesis and no eligibility error. The `serialized`
// clause on the field is a separate compile error (§9.8: "A `serialized` clause
// on a member of a type that declares no serialization conformance … is a
// compile error."). But the ELIGIBILITY error itself must not fire.
// (We test only that a plain record with no conformance declaration compiles.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C end C; record R(X: int, Obj: C);

// NODIAG-NOT: error
// NODIAG-NOT: §
