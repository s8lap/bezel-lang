// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, arrayspan_phantom_member_rejected_4_15).
//
// (a) Phantom member on ArraySpan<T> → compile error.

module M; class C public static method Test() const arr: Array<int> := [1]; var sp := arr.AsSpan(); sp.Bogus(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Bogus` on `ArraySpan<T>`; documented surface: Length, IsEmpty, IndexOf(), Contains(), SequenceEqual(), Slice(), AsReadOnly()/Reverse()/Fill()/CopyFrom()/CopyTo() [writable spans], Clear()/MoveFrom()/MoveTo() [owning class Span<T>]; when T=byte: Read*/TryRead*/Write*/TryWrite*{Int16,UInt16,Int32,UInt32,Int64,UInt64,Float,Double}{LittleEndian,BigEndian} (stdlib §4.14/§4.15)
