//! case: s26_4_0007
//! doc: spec
//! section: 26.4
//! title: a block-level T local swaps elements of an Array<long> receiver
//! rule: The block-level type parameter is in scope throughout the block. ... Each member of a generic extension block is read ... as a generic method that declares the block's parameters before its own — bound by the receiver at a call or a property read (§26.5 step 2)
//! cites: 26.5, 14.1, 3.6
//! expect: run
//! output: 20000000000
//!   10000000000
//!   10000000000
//!   20000000000
//!   402
module Conf;
class Pt public Id: int; public constructor(i: int) Id := i; end; end Pt;
record Rec(A: int, B: string);
choice Ch
  Ca(x: int),
  Cb(s: string);
end Ch;
extension Array<T>
  public method ASwap()
    var t: T := this[0]; this[0] := this[1]; this[1] := t;
  end ASwap;
  public method ABody(): int
    var t: T := this[0]; this[0] := this[1]; this[1] := t;
    return this.Count;
  end ABody;
end;
class Program
  public static method Show(x: long) Console.WriteLine(x); end Show;
  public static method Main(): int
    var a: Array<long> := [10000000000L, 20000000000L];
    var i := 0;
    while i < 201 do a.ASwap(); i := i + 1; end;
    Show(a[0]); Show(a[1]);
    i := 0;
    var n := 0;
    while i < 201 do n := n + a.ABody(); i := i + 1; end;
    Show(a[0]); Show(a[1]);
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
