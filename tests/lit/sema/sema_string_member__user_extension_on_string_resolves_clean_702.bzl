// Ported from tests/sema/string_member_diagnostic_test.cpp
//   (BEZEL_TEST sema_string_member, user_extension_on_string_resolves_clean_702).
//
// fix #702 / §26.5: a user-defined extension method on `string` must resolve
// cleanly — the "not yet supported" catch-all must NOT fire before extension
// lookup is attempted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; extension string   public method IsBlank(): bool => this.Length = 0; end; class C   public static method Main(): int     var s := "hello";     var b := s.IsBlank();     return 0;   end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
