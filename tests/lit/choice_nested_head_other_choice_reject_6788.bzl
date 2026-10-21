// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #6788 -- spec §35.5 *Case patterns*, v0.340: "a nested head names a
// case of the choice that types the slot (`Wrap(W(var a, var b))` over a `Box`
// whose `Wrap` payload is an `Outer`, `W` being `Outer`'s case; ...)".
// `Outcome.Fetched` is a case of `Outcome`, not of the `Outer` that types the
// `Wrap` slot. Before the fix it compiled and the arm ran for an `Outer.X`.

module M;
choice Outcome NotModified, Fetched(Status: int), Failed(Msg: string); end Outcome;
choice Outer X, Y, W(A: int, B: int); end Outer;
choice Box Wrap(O: Outer), Empty; end Box;
class Program
  public static method Main()
    var b := Box.Wrap(Outer.X);
    var s := (case b of Wrap(Outcome.Fetched(var c)) => "f", _ => "o");
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: nested pattern head `Outcome.Fetched` is not a case of `Outer`, the choice that types this slot (§35.5
    Console.WriteLine(s);
  end Main;
end Program;
