// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_import_gate_631_sema_test.cpp
//   (BEZEL_TEST sema_serialization_import_gate_631, binary_serializer_type_annotation_no_import_spec_2_15).
//
// §2.15 / #631 (a): BinarySerializer as a type annotation without import →
// compile error.

module M; class C public static method Main(): int var s: BinarySerializer := new BinarySerializer(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `BinarySerializer` is declared in `System.Serialization` and requires `import System.Serialization;`
// CHECK-SAME: (§2.15, #631)
