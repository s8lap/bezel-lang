// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/string_member_diagnostic_test.cpp
//   (BEZEL_TEST sema_string_member, unknown_string_member_still_errors_after_702).
//
// fix #702 / §26.5 canary: a genuinely unknown string member (not an
// extension) must STILL produce a diagnostic — the fix must not weaken the
// error path.

module M; extension string   public method IsBlank(): bool => this.Length = 0; end; class C   public static method Main(): int     var s := "hello";     var x := s.Typo;     return 0;   end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: string member `.Typo` is not yet supported in codegen
