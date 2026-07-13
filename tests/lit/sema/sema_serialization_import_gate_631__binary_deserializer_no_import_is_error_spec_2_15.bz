// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_import_gate_631_sema_test.cpp
//   (BEZEL_TEST sema_serialization_import_gate_631, binary_deserializer_no_import_is_error_spec_2_15).
//
// §2.15 / #631 (a): using BinaryDeserializer without import → compile error.

module M; enum Color: ISerializable, IDeserializable Red, Green; end Color; class C public static method Main(): int using var s := new BinarySerializer(); Color.Red.Serialize(s); using var bytes := s.ToBytes(); using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `BinarySerializer` is declared in `System.Serialization` and requires `import System.Serialization;`
// CHECK-SAME: (§2.15, #631)
