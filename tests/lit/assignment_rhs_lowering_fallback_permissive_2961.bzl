// Companion to assignment_rhs_lowering_fallback_warns_2961.bzl, covering the
// OTHER half of #5327's split.
//
// #5327 (codegen, LlvmBackend_Stmts.cpp Emitter::lowerStmt, v0.315) split each
// of the two `if (!rhs) { … return false; }` assignment-RHS sites into an
// if/else that reports the SAME degrade with one of two TEXTS: when
// `builtinReceiverOfMemberCall` resolved a built-in receiver it appends
// "— ChannelSender.Send produces no value here", and the `else` — this case —
// keeps the original undecorated "— #2961". The sibling case pins the named
// branch on both arms; this one pins the fallback branch on both, so the split
// cannot lose a branch without a lit failure.
//
// REACHING THE FALLBACK. The `else` is taken exactly when the RHS is NOT a
// built-in member call. An unknown member on a §3/#605-permissive Object-typed
// receiver is the shape that lands there: sema accepts the member access under
// the permissive surface, lowerExpr has no dispatch to lower it to a value, and
// `builtinReceiverOfMemberCall` — which only recognises a RECEIVER of built-in
// type — returns empty, so the undecorated text is reported. Verified
// empirically: this file prints the bare "#2961" sentence below, and the
// #2477 permissive-acceptance note alongside it names the same shape.
//
// BOTH ARMS, as in the sibling: the localSlot arm via `x := …` on a local, and
// the generic lvalueAddr arm via `this.f := …` on a class field. Two CHECK-DAG
// lines, one per arm, so dropping either arm's fallback site fails loudly.
//
// ACCEPT-WITH-WARNING control (same as the sibling): bezelc -c must still exit
// zero — the program IS legal under §3/#605 — while printing the internal
// Warning that names the dropped store.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class C
  public f: int;
  public constructor() f := 0; end constructor;
  public method M(o: Object): int
    var x: int := 7;
    x := o.NoSuchMember(42);
    this.f := o.NoSuchMember(43);
    return x + this.f;
  end M;
end C;

// The fallback text is the UNDECORATED one — no "produces no value here", and
// no built-in name. Both arms report it, hence two CHECK-DAG lines.
// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: assignment RHS expression failed to lower to a value for this assignment's target (store dropped — target retains its previous value) — #2961
// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: assignment RHS expression failed to lower to a value for this assignment's target (store dropped — target retains its previous value) — #2961
