// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_import_gate_631_sema_test.cpp
//   (BEZEL_TEST sema_serialization_import_gate_631, binary_deserializer_type_annotation_no_import_spec_2_15).
//
// §2.15 / #631 (a): BinaryDeserializer as a type annotation without import →
// compile error.

module M; class C public method Deser(d: BinaryDeserializer): int return 0; end Deser; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `BinaryDeserializer` is declared in `System.Serialization` and requires `import System.Serialization;`
// CHECK-SAME: (§2.15, #631)
