// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, builtin_string_methods_clean).
//
// Builtin string methods (no sema MemberSymbol) stay permissive (§9.6).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(s: string): int var t := s.Trim(); var u := s.ToUpper(); return s.Length; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
