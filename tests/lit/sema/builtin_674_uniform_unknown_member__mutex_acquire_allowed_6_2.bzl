// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, mutex_acquire_allowed_6_2).
//
// (b) Mutex.Acquire() still compiles without error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Threading; class C public static method Test() using var m := new Mutex(); using var guard := m.Acquire(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
