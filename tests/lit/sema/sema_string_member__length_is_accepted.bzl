// Ported from tests/sema/string_member_diagnostic_test.cpp
//   (BEZEL_TEST sema_string_member, length_is_accepted).
//
// Source built by hand from the file's local `check(body)` C++ helper
// (`"module M; class C public static method Main(): int " + body + "
// end Main; end C;"`) — the extractor's harness-name table treats a bare
// `check(...)` call as ALREADY being the full harness invocation (the
// generic name collides with a genuinely full-source harness convention
// used elsewhere), so it resolved the raw method-body fragment as if it
// were the whole compilation unit and produced a bogus parse-error mismatch;
// this .bzl applies the real wrapping by hand.
//
// §4.6, stdlib §9.6: `.Length` is a known intrinsic → sema accepts it (no error).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var s := "hello"; return s.Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
