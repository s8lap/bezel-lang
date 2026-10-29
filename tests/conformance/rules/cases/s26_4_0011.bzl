//! case: s26_4_0011
//! doc: spec
//! section: 26.4
//! title: a block-level T local swaps elements of an Array<string> receiver (T-free swap)
//! rule: The block-level type parameter is in scope throughout the block. ... Each member of a generic extension block is read ... as a generic method that declares the block's parameters before its own — bound by the receiver at a call or a property read (§26.5 step 2)
//! cites: 26.5, 14.1, 3.6
//! expect: run
//! output: y
//!   201
module Conf;
extension Array<T>
  public method Sw(): int
    var t: T := this[0]; this[0] := this[1]; this[1] := t;
    return 1;
  end Sw;
  public property P: int => this.Sw();
  public property Q: int => this.Count;
end;
class Program
  public static method Main(): int
    var s: Array<string> := ["x", "y"];
    var i := 0; var n := 0;
    while i < 201 do n := n + s.Sw(); i := i + 1; end;
    Console.WriteLine(s[0]); Console.WriteLine(n);
    return 0;
  end Main;
end Program;
