// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, binaryserializer_iserializer_surface_allowed_2_14).
//
// (b) BinarySerializer ISerializer methods still compile without error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Serialization; class C public static method Test() using var s := new BinarySerializer(); s.WriteInt(1); s.WriteUInt(2); s.WriteFloat(3.0); s.WriteBool(true); s.WriteString("hi"); s.WriteChar('A'); s.WriteNull(); s.BeginArray(0); s.EndArray(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
