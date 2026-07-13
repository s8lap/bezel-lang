// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, binaryserializer_phantom_member_rejected_2_15).
//
// (a) Phantom member on BinarySerializer → compile error.

module M; import System.Serialization; class C public static method Test() using var s := new BinarySerializer(); s.Bogus(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Bogus` on `BinarySerializer`; documented surface: ToBytes(), WriteInt/UInt/Float/Decimal/Bool/String/Char/WriteNull(), BeginObject/EndObject(), WriteFieldName(), BeginArray/EndArray(), Dispose() (stdlib §2.14–2.15)
