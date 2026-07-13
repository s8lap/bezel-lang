// Ported from tests/sema/serialization_import_gate_631_sema_test.cpp
//   (BEZEL_TEST sema_serialization_import_gate_631, binary_serializer_with_import_accepted_spec_2_15).
//
// §2.15 / #631 canonical form: with import, BinarySerializer usage → no error.
// (Regression guard for the existing #550 tests.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Serialization; enum Color: ISerializable, IDeserializable Red, Green; end Color; class C public static method Main(): int using var s := new BinarySerializer(); Color.Red.Serialize(s); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
