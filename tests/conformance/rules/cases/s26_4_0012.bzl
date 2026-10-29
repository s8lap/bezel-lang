//! case: s26_4_0012
//! doc: spec
//! section: 26.4
//! title: an extension on a user generic class Box<T> binds T per receiver instantiation
//! rule: The block-level type parameter is in scope throughout the block. ... Each member of a generic extension block is read ... as a generic method that declares the block's parameters before its own — bound by the receiver at a call or a property read (§26.5 step 2)
//! cites: 26.5, 14.1, 3.6
//! expect: run
//! output: y
//!   x
//!   201
//!   2
module Conf;
class Box<T>
  public _a: Array<T>;
  public constructor(a: Array<T>) _a := a; end constructor;
end Box;
class Pt public Id: int; public constructor(i: int) Id := i; end; end Pt;
extension Box<T>
  public method Sw(): int
    var t: T := this._a[0]; this._a[0] := this._a[1]; this._a[1] := t;
    return 1;
  end Sw;
  public property P: int => this.Sw();
end;
class Program
  public static method Main(): int
    var s: Array<string> := ["x", "y"];
    var b := new Box<string>(s);
    var i := 0; var n := 0;
    while i < 201 do n := n + b.Sw(); i := i + 1; end;
    Console.WriteLine(s[0]); Console.WriteLine(s[1]); Console.WriteLine(n);
    using var p1 := new Pt(1); using var p2 := new Pt(2);
    var c: Array<Pt> := [p1, p2];
    var bc := new Box<Pt>(c);
    i := 0;
    while i < 201 do n := n + bc.Sw(); i := i + 1; end;
    Console.WriteLine(c[0].Id);
    return 0;
  end Main;
end Program;
