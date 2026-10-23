// RUN: %bezelc -c -o %t.lib.o %S/xmod_static_interface_2294_lib.bzl
// RUN: %not %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the second compile is expected to fail
// — see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.
// `%S` = this test file's directory, so the first line builds the dependency
// artifact the second imports.)
//
// Issue #2294 / spec §11.2, §11.8 (v0.176) — the NO-FALSE-ACCEPT guard for the
// cross-module static-interface conformance fix. The fix widens where the
// interface member's signature is READ FROM (the imported side tables, since an
// imported member has no AST decl); it must not widen WHAT MATCHES. Here the
// witness `Ping(n: string)` contradicts the imported interface's declared
// `Ping(n: int)`, so §11.2's exact-match rule for statics must still reject it.
//
// This asserts the diagnostic's CONTENT — the rule wording and both cited spec
// sections — not merely that compilation failed: the gtest harness maps a
// compiler CRASH to the same signal as a clean reject (issue #2254), so a
// content match is what distinguishes a real §11.2/§11.8 diagnostic from an
// internal-compiler-error escape. The escape guard below pins that too.
// (#6620: that guard used to name the codegen-only `internal:` marker —
// codegen is gated on a clean front end, so on a row that rejects in the front
// end it named a needle that could never fire; it now names the front end's
// own ICE marker.)
module SiApp;
import SiLib2294;

public class Bad: IPing
  public static method Ping(n: string): int => 1;
end Bad;

public class C
  public static method Main(): int => 0;
end C;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape and the rule text.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Bad` implements static interface member `Ping` of `IPing` with a different signature
// CHECK-SAME: (§11.2 / §11.8)
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
