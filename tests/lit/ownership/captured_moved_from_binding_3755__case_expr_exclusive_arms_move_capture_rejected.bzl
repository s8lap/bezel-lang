// UPDATED by #3989/#3990 (§28.3 K7.4, issue #3999) — the `case`-expression
// twin of `captured_moved_from_binding_3755__if_expr_exclusive_arms_move_
// capture_rejected.bzl`; see that file's header comment for the full
// analysis (the gap, the measured pre-existing double-free on the untested
// `Run(true)` path, and why the rejection is sound). This file used to be
// `..._accepted.bzl`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class C public static method Consume(x: owned R): () -> int x.Dispose(); return lambda(): int => 99; end Consume; public static method Run(flag: bool): int var p := new R(); var f: () -> int := (case flag of true => Consume(move p), _ => lambda(): int => p.V()); return f(); end Run; public static method Main(): int Console.WriteLine(Run(false).ToString()); return 0; end Main; end C;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: closure captures
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `p` would be dropped here without disposal (§28.3 — consider `using var`, explicit `.Dispose()`, `move`, or `return`)
// CHECK-NOT: closure captures
