// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, binaryserializer_tobytes_allowed_2_15).
//
// (b) BinarySerializer.ToBytes() still compiles without error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Serialization; class C public static method Test() using var s := new BinarySerializer(); s.BeginObject(1); s.WriteFieldName("x"); s.WriteInt(42); s.EndObject(); using var bytes := s.ToBytes(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
