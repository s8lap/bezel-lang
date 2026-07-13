// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, builtin_console_math_clean).
//
// Builtin static classes Console / Math stay permissive (§2.3 / stdlib §9).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(): int Console.WriteLine("hi"); var x := Math.Sqrt(4.0); return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
