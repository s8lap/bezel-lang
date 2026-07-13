// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, hand_written_serialize_suppresses_synthesis_spec_9_8).
//
// §9.8: a record with a hand-written `Serialize` method suppresses synthesis
// on that direction, so the record is always valid even if ineligible, provided
// it supplies its own implementation. A class-typed field no longer disqualifies.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C end C; record R(X: int, Obj: C): ISerializable public method Serialize(s: ISerializer) end Serialize; end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
