//! case: s26_4_0015
//! doc: spec
//! section: 26.4
//! title: an extension on a two-parameter generic class binds A and B independently
//! rule: The block-level type parameter is in scope throughout the block. ... Each member of a generic extension block is read ... as a generic method that declares the block's parameters before its own — bound by the receiver at a call or a property read (§26.5 step 2)
//! cites: 26.5, 14.1, 3.6
//! expect: run
//! output: y
//!   201
//!   n
//!   2.5
//!   1
module Conf;
class P2<A, B>
  public fa: Array<A>; public fb: Array<B>;
  public constructor(a: Array<A>, b: Array<B>) fa := a; fb := b; end constructor;
end P2;
extension P2<A, B>
  public method SwapB(x: A): int
    var t: B := this.fb[0]; this.fb[0] := this.fb[1]; this.fb[1] := t;
    return 1;
  end SwapB;
  public method SwapBoth(): int
    var t: B := this.fb[0]; this.fb[0] := this.fb[1]; this.fb[1] := t;
    var u: A := this.fa[0]; this.fa[0] := this.fa[1]; this.fa[1] := u;
    return 2;
  end SwapBoth;
end;
extension Dictionary<K, V>
  public method Touch(): int
    var n := 0;
    for var e in this do n := n + 1; end;
    return n;
  end Touch;
end;
class Program
  public static method Main(): int
    var a: Array<int> := [1, 2]; var b: Array<string> := ["x", "y"];
    var p := new P2<int, string>(a, b);
    var i := 0; var n := 0;
    while i < 201 do n := n + p.SwapB(7); i := i + 1; end;
    Console.WriteLine(b[0]); Console.WriteLine(n);
    var a2: Array<string> := ["m", "n"]; var b2: Array<double> := [1.5, 2.5];
    var p2 := new P2<string, double>(a2, b2);
    i := 0;
    while i < 201 do n := n + p2.SwapBoth(); i := i + 1; end;
    Console.WriteLine(a2[0]); Console.WriteLine(b2[0]);
    var d := new Dictionary<string, string>();
    d.Add("k", "v");
    Console.WriteLine(d.Touch());
    return 0;
  end Main;
end Program;
