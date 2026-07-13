// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, char_repeat_is_rejected).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (`"module M; class C public static method Main(): int " + body + "
// end Main; end C;"`) — the extractor's harness-name table treats a bare
// `checkBody(...)` call as ALREADY being the full harness invocation (the
// generic name collides with a genuinely full-source harness convention
// used elsewhere), so it resolved the raw method-body fragment as if it
// were the whole compilation unit; this .bzl applies the real wrapping by
// hand.
//
// §3.4 / stdlib §9.5: char has no static method `Repeat`. The real API is
// string.Repeat(c: char, count: int) at stdlib §9.6.

module M; class C public static method Main(): int var rep := char.Repeat('x', 3); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type 'char' has no static method or property 'Repeat'
// CHECK-SAME: (§3.4)
