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
// built-in member call. An expression sema ADMITS and codegen cannot lower is
// the shape that lands there: the RHS is an indexer read `o[42]` on a bare
// type parameter `T` — §3.6 rule 2 / §14.2 leave a bare type parameter's
// member set open, so sema admits the read, while the monomorphized clone at
// `T = int` has no indexer to dispatch to, so `lowerExpr` returns null;
// `builtinReceiverOfMemberCall` — which only recognises a RECEIVER of built-in
// type — returns empty, so the undecorated text is reported. Verified
// empirically: this file prints the bare "#2961" sentence below.
//
// ⚠ THE TRIGGER WAS RE-BASED BY fix #7973 (§8.1, v0.350). This case used to
// use `o.NoSuchMember(42)` on an `Object`-typed receiver — the §3/#605
// permissive "unknown member" acceptance. §8.1 declares the root `Object`'s
// public surface ("The declaration above defines the public surface"), and
// §26.5 resolves a member against the receiver's declared type, so that shape
// is now a COMPILE ERROR and never reaches codegen. The subject of this case —
// the UNDECORATED fallback branch — is unchanged; only the shape that reaches
// it moved, to the one legal shape that still does.
//
// BOTH ARMS, as in the sibling: the localSlot arm via `x := …` on a local, and
// the generic lvalueAddr arm via `this.f := …` on a class field. Two CHECK-DAG
// lines, one per arm, so dropping either arm's fallback site fails loudly.
//
// ACCEPT-WITH-WARNING control (same as the sibling): bezelc -c must still exit
// zero — the program IS legal — while printing the internal Warning that names
// the dropped store.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class C
  public f: int;
  public constructor() f := 0; end constructor;
  public method M<T>(o: T): int
    var x: int := 7;
    x := o[42];
    this.f := o[43];
    return x + this.f;
  end M;
end C;

// The fallback text is the UNDECORATED one — no "produces no value here", and
// no built-in name. Both arms report it, hence two CHECK-DAG lines.
// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: assignment RHS expression failed to lower to a value for this assignment's target (store dropped — target retains its previous value) — #2961
// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: assignment RHS expression failed to lower to a value for this assignment's target (store dropped — target retains its previous value) — #2961
