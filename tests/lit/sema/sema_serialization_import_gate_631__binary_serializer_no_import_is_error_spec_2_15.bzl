// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_import_gate_631_sema_test.cpp
//   (BEZEL_TEST sema_serialization_import_gate_631, binary_serializer_no_import_is_error_spec_2_15).
//
// §2.15 / #631 (a): using BinarySerializer without import → compile error
// naming the required import.

module M; enum Color: ISerializable, IDeserializable Red, Green; end Color; class C public static method Main(): int using var s := new BinarySerializer(); Color.Red.Serialize(s); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `BinarySerializer` is declared in `System.Serialization` and requires `import System.Serialization;`
// CHECK-SAME: (§2.15, #631)
