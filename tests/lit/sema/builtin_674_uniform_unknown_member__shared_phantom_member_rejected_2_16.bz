// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, shared_phantom_member_rejected_2_16).
//
// (a) Phantom member on Shared<T> → compile error.

module M; class Foo end Foo; class C public static method Test(owned s: Shared<Foo>) s.Bogus(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Bogus` on `Shared<T>`; documented surface: Clone(), Get(), Downgrade(), Dispose() (stdlib §2.16)
