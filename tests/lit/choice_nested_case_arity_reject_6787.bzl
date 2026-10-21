// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #6787 -- spec §35.5 *Case patterns*, v0.340: "A payload case is matched
// by a positional destructuring pattern `CaseName(slot, ...)` with the case's
// exact payload arity." `W` carries two slots, so the NESTED one-slot pattern
// `W(_)` is refused (it compiled and matched by tag), and the bare-case hint
// spells the discard per slot, `W(_, _)`.

module M;
choice Outer X, Y, W(A: int, B: int); end Outer;
choice Box Wrap(O: Outer), Empty; end Box;
class Program
  public static method Main()
    var b := Box.Wrap(Outer.W(1, 2));
    var s := (case b of Wrap(W(_)) => "x", _ => "o");
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: case-destructuring pattern `W(...)` binds 1 slot(s) but case `W` has 2 payload field(s); the slot count must match (§35.5)
    var t := (case b of Wrap(Outer.W) => "x", _ => "o");
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: Payload case `W` requires a payload pattern: write `W(2 slots)`, or `W(_, _)` to discard (§35.5)
    Console.WriteLine(s);
    Console.WriteLine(t);
  end Main;
end Program;
