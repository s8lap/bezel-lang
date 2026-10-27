// UPDATED by #3989/#3990 (§28.3 K7.4, issue #3999). This file used to be
// `..._accepted.bzl` and assert `bezelc -c` exits zero with no diagnostic —
// on the premise that a `move` and a capturing lambda literal sitting in
// MUTUALLY EXCLUSIVE arms of the §20 if-atom are safe together, since only
// one arm ever runs.
//
// That premise is right about the CAPTURE (on the non-consuming path `p` is
// definitely live, and #3755's own "closure captures ... whose owned value
// ..." diagnostic correctly stays silent — see the CHECK-NOT below). It is
// WRONG about the whole construct's scope-end obligation: with `flag = true`
// (never exercised anywhere in this fixture) `p` is genuinely moved into
// `Consume`, which disposes it, and `Run`'s own end-of-method auto-drop —
// before #3989/#3990, unaware that `p` was ever touched by the conditional,
// because `recordMovesIn` had no `ConditionalExpr`/`CaseExpr` arm at all —
// disposes the SAME (already-freed) `R` a second time. Measured
// (`--sanitize=address`, calling `Run(true)`): heap-use-after-free, freed
// once inside `bz$M.C$Consume`, touched again inside `bz$M.C$Run`. Confirmed
// pre-existing and unrelated to #3971.
//
// #3989/#3990 gives `recordMovesIn` the missing arms and folds each arm's
// exit state via the SAME `joinInto` meet the real CFG `if`-statement join
// already uses (§28.6 issue #338 / #1626's "Owned ⊓ MovedFrom ⇒
// Owned+maybeConsumed"). `p` now correctly joins to Owned+maybeConsumed
// after the construct, and reaching Run's end-of-method scan in that state
// is the ALREADY-SHIPPED §28.3 "would be dropped here without disposal"
// ERROR — exactly what the equivalent real STATEMENT form (`if flag then
// Sink.Take(move x); end;`, no else, no post-handling) has always correctly
// rejected. This program genuinely is unsafe to auto-drop unconditionally,
// so it is now REJECTED rather than silently accepted. #3999 tracks the
// residual (no way to write this safely today — the K3.4 marked-arm-transfer
// per-path disposal machinery covers only `owned x` as the bare arm value,
// not a `move` embedded inside a larger arm expression).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class C public static method Consume(x: owned R): () -> int x.Dispose(); return lambda(): int => 99; end Consume; public static method Run(flag: bool): int var p := new R(); var f: () -> int := (if flag then Consume(move p) else lambda(): int => p.V()); return f(); end Run; public static method Main(): int Console.WriteLine(Run(false).ToString()); return 0; end Main; end C;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: closure captures
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `p` would be dropped here without disposal (§28.3 — consider `using var`, explicit `.Dispose()`, `move`, or `return`)
// CHECK-NOT: closure captures
