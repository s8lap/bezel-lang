// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_import_gate_631_sema_test.cpp
//   (BEZEL_TEST sema_serialization_import_gate_631, binary_deserializer_owned_buffer_arg_is_error_spec_2_15).
//
// §2.15 / #631 (b): passing the raw OwningBuffer<byte> to new BinaryDeserializer
// instead of the required ReadOnlySpan<byte> → ctor-arg type error.

module M; import System.Serialization; enum Color: ISerializable, IDeserializable Red, Green; end Color; class C public static method Main(): int using var s := new BinarySerializer(); Color.Red.Serialize(s); using var bytes := s.ToBytes(); using var d := new BinaryDeserializer(bytes); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new BinaryDeserializer(...)` expects `ReadOnlySpan<byte>` as its first argument (stdlib §2.15); got `OwningBuffer<byte>` — call `.AsReadOnlySpan()` first
// CHECK-SAME: (§2.15, #631)
