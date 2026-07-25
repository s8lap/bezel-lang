// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail, and
// wrapping it in `not` lets its diagnostics flow through lit's pipe to
// FileCheck — see tests/lit/lit.cfg.py and README.md.)
//
// Spec §7.4 "How narrowing is established / invalidated" (v0.226, issue #2152,
// residual of #2118): narrowing is flow-sensitive — "at each program point the
// compiler tracks, per local, whether its value is known non-null." After a
// loop the state is the JOIN of every edge that can reach the after-loop point:
// the zero-iteration path (condition false on entry), the body fall-through
// path (condition false after a full body), AND each `break`/`continue` edge.
// A `break` exits the loop directly; a `continue` jumps to the condition
// re-check, which on cond-false also exits — so an edge that leaves a local
// possibly-null makes the after-loop state possibly-null too.
//
// #2118's join intersected only the entry and body fall-through-exit sets, so
// when the body INVALIDATED an entry narrowing before a conditional `break`/
// `continue` and RE-ESTABLISHED it afterward, the fall-through exit kept the
// local narrowed and a bare member access after the loop was wrongly ACCEPTED
// — an unsound over-narrowing on the break/continue path. This reject track
// pins those edges (which a behavior/accept test cannot express — the fix
// REJECTS these). The fix folds each break/continue edge snapshot into the
// after-loop intersection; the programs below fail without it (they compiled
// cleanly) and pass with it (rejected per §7.4).
module M;
class C
  // Edge 1 — `while` + `break`: `s` is narrowed non-null by the outer `if s =
  // null then return` guard (§7.4 early-exit establishment). The body
  // invalidates `s` (`s := other`, `other: string?`), then conditionally
  // `break`s — the break edge leaves `s` possibly-null — and re-establishes it
  // (`s := seed`) on the fall-through path. The fall-through exit keeps `s`
  // narrowed, but the break edge does not, so the after-loop `.Length` must be
  // rejected.
  public static method WhileBreakEdge(s: string?, other: string?, seed: string,
                                      cond: bool, brk: bool): int
    if s = null then return -1; end;
    while cond do
      s := other;
      if brk then
        break;
      end;
      s := seed;
    end;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
  end WhileBreakEdge;

  // Edge 2 — `while` + `continue`: same shape, but the edge is a `continue`.
  // `continue` jumps to the condition re-check, which on cond-false exits the
  // loop, so the continue-edge state is also a possible after-loop state and
  // the after-loop `.Length` must be rejected.
  public static method WhileContinueEdge(s: string?, other: string?,
                                         seed: string, cond: bool,
                                         cont: bool): int
    if s = null then return -1; end;
    while cond do
      s := other;
      if cont then
        continue;
      end;
      s := seed;
    end;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
  end WhileContinueEdge;

  // Edge 3 — `for` + `break`: the same edge-state join applies to `for` bodies
  // (which may also `break`). The break edge leaves `s` possibly-null after
  // the in-body invalidation, so the after-loop `.Length` must be rejected.
  public static method ForBreakEdge(s: string?, other: string?, seed: string,
                                    brk: bool): int
    if s = null then return -1; end;
    for var i in 0..2 do
      s := other;
      if brk then
        break;
      end;
      s := seed;
    end;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
  end ForBreakEdge;
end C;
