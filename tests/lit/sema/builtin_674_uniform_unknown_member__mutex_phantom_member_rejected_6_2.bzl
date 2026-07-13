// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, mutex_phantom_member_rejected_6_2).
//
// (a) Phantom member on Mutex → compile error.

module M; import System.Threading; class C public static method Test() using var m := new Mutex(); m.Bogus(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Bogus` on `Mutex`; documented surface: Acquire(), TryAcquire(timeout: TimeSpan), AcquireAsync(), TryAcquireAsync(), Dispose() (stdlib §6.2)
