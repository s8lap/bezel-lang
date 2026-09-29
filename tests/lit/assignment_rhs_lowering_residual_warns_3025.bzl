// fix #3025 (§6.6 / §7.7 / §28.3 / §28.9, v0.240): diagnostic pin for
// the two null-RHS exits in AssignmentStmt that #2961's generic `:=` arms do
// not reach. A §6.9 ChannelSender<int>.Send call returns unit and therefore
// produces no lowered LLVM value. Before #3025 both dedicated arms returned
// silently, leaving the target unchanged; after #3025 each successful compile
// emits one guarded Warning naming the degradation.
//
// §6.6, verbatim: "`e` is evaluated — and the setter invoked — only when the
// assignment actually occurs." The `??=` target is nullable and starts null so
// the RHS arm is the emitted assignment path. §28.3 K8, verbatim: "A full
// expression is a maximal expression — not a subexpression of any larger
// expression — at the enumerated positions (initializers, assignment RHSs,
// expression statements, each evaluation of a condition or guard, `for`
// sources, `return`/`throw`/`yield return` operands, `case` scrutinees)."
//
// ACCEPT-WITH-WARNING: both diagnostics are Warnings, so object emission must
// still succeed. The companion AOT test checks the retained values, RHS side
// effects, and successful non-null assignment paths.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module N3025Lit;
class C
  public static method Main(): int
    using var a := new Array<int>(1, 17);
    var x: int? := null;
    using var ch := new Channel<int>();
    using var s := ch.NewSender();
    a[0] := s.Send(42);
    x ??= s.Send(43);
    return a[0] + (x ?? 0);
  end Main;
end C;

// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: indexer-setter RHS expression failed to lower (degraded — target retains its previous value) — #3025
// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: ??= short-circuit RHS expression failed to lower (degraded — target retains its previous value) — #3025
