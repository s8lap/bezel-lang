// fix #4626 + #4701: the `Severity::Warning` renderer's byte format must
// carry the literal "warning: " severity prefix so the conformance runner's
// `\bwarn(?:ing)?\b` regex (tests/conformance/rules/runner.py) can match
// spec-required warnings. Before #4626 the driver emitted `path:line:col:
// <message>` and the regex could not see the warning, so ~30 warn-tier
// corpus rows sat at FAIL despite the compiler producing the diagnostic.
// Centralizing the prefix in Driver.cpp's `renderDiagnostic` is what #4626
// does; this test pins the byte format at the stderr emission boundary so
// future emission paths cannot regress by including the warning word
// inline in their message text and bypassing the regex (which would create
// the same false-positive class #4626 fixed).
//
// The accepted-with-warning program shape is the canonical owned-iter-var
// auto-drop (§29.2 / §28.3, v0.157): the body never discharges the owned
// value, so it is implicitly disposed at loop end with a must-use WARNING
// (NOT a hard error). The CHECK below pins the severity prefix at byte
// position 0 of the rendered diagnostic — the prefix is non-optional.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Box public constructor() end constructor; destructor end destructor; end Box; class C public static method Gen(): IOwningEnumerable<Box> yield return new Box(); end Gen; public static method Main(): int for var x in Gen() do end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: warning: owned value in `x` is implicitly disposed at the end of each loop iteration
