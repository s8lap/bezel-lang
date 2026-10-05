// fix #2961 (codegen, LlvmBackend_Stmts.cpp Emitter::lowerStmt, v0.240):
// assignment-position twin of #2434 (local-decl) and #2491 (return). When
// `lowerExpr` returned null for the RHS of a `:=` statement, both the
// localSlot arm and the generic lvalueAddr fallback in lowerStmt bailed
// silently with `return false;` — the store did not happen, the target kept
// its previous value, and the program emitted NO diagnostic of any severity.
// This pin covers both arms' Warning: the localSlot arm via
// `x := sender.Send(42);` on a §6.9 ChannelSender, and the generic
// lvalueAddr arm via `this.f := sender.Send(43);` on a class field. Each
// Send call lowers as a unit (§6.9, stdlib §6.9 Send yields nothing), so
// `lowerExpr` returns null for the RHS — exactly the "produced no value"
// shape #2961 cites, and SHARED with the local-decl twin (`var x := sender
// .Send(42);` in the same method body prints #2434's warning for the
// identical underlying lowerExpr failure).
//
// ACCEPT-WITH-WARNING control (mirrors
// local_decl_zero_init_fallback_warns_2434.bzl and
// return_value_lowering_fallback_warns_2491.bzl): bezelc -c must still
// exit zero — the program IS legal — but it now emits an internal Warning
// naming the failed lowering, where before this fix it emitted NONE.
//
// WHY THIS PROGRAM REACHES THE FALLBACK (verified empirically on f6eba55,
// before the fix the program printed `0` for both `x` and `f` with NO
// diagnostic — the silent drop the fix closes): `sender.Send(42)` is a
// §6.9 unit-returning built-in member call (§4.7 — `lowerExpr` returns
// null because the call result has no lowered value), and the §6.9 sender
// is a built-in type with no user declaration. The local-decl twin
// (`var s := sender.Send(42);`) in the same method body prints #2434's
// warning for the identical underlying lowerExpr failure — the loud-vs-
// silent inconsistency between the two twins was load-bearing evidence for
// the #2961 report, exactly as #2491 cited for its return-path twin. This
// is why the new diagnostic is Severity::Warning, not Error: the program
// IS a legal compile (sema accepts the call by the §6.9 surface), and an
// unconditional Error would reject this and the related §3/#605 corpus
// programs sharing this shape — turning a currently-legal compile into a
// hard failure, the false-reject #2434 warned against checking for before
// choosing Error.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class C
  public f: int;
  public constructor() f := 0; end constructor;
  public method M(ch: Channel<int>): int
    using var sender: ChannelSender<int> := ch.NewSender();
    var x: int := 7;
    x := sender.Send(42);
    this.f := sender.Send(43);
    return x + this.f;
  end M;
end C;

// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: assignment RHS expression failed to lower to a value for this assignment's target (store dropped — target retains its previous value) — ChannelSender.Send produces no value here (#2961)
// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: assignment RHS expression failed to lower to a value for this assignment's target (store dropped — target retains its previous value) — ChannelSender.Send produces no value here (#2961)
