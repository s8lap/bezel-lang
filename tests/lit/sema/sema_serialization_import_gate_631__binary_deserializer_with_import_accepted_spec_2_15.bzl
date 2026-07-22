// Ported from tests/sema/serialization_import_gate_631_sema_test.cpp
//   (BEZEL_TEST sema_serialization_import_gate_631, binary_deserializer_with_import_accepted_spec_2_15).
//
// §2.15 / #631 canonical form: with import, BinaryDeserializer usage → no
// error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Serialization; enum Color: ISerializable, IDeserializable Red, Green; end Color; class C public static method Main(): int using var s := new BinarySerializer(); Color.Red.Serialize(s); using var bytes := s.ToBytes(); using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); var v := Color.Deserialize(d); return (if v = Color.Red then 1 else 0); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
