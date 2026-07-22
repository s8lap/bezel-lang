// Ported from tests/sema/serialization_import_gate_631_sema_test.cpp
//   (BEZEL_TEST sema_serialization_import_gate_631, binary_deserializer_asreadonly_span_accepted_spec_2_15).
//
// §2.15 / #631 (b): with import + AsReadOnlySpan() — canonical; no error.
// Uses the enum form (same as binary_deserializer_with_import_accepted_spec_2_15)
// to confirm that the AsReadOnlySpan() arg is accepted by the ctor check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Serialization; enum Status: ISerializable, IDeserializable Active, Inactive; end Status; class C public static method Main(): int using var s := new BinarySerializer(); Status.Active.Serialize(s); using var bytes := s.ToBytes(); using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); var v := Status.Deserialize(d); return (if v = Status.Active then 1 else 0); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
