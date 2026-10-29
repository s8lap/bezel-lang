//! case: s26_4_0013
//! doc: spec
//! section: 26.4
//! title: an extension with where T: class and a nested Array<Array<T>> head both bind T per receiver
//! rule: The block-level type parameter is in scope throughout the block. ... Each member of a generic extension block is read ... as a generic method that declares the block's parameters before its own — bound by the receiver at a call or a property read (§26.5 step 2)
//! cites: 26.5, 14.1, 3.6
//! expect: run
//! output: 2
//!   c
//!   a
//!   2.5
//!   1.5
module Conf;
class Pt public Id: int; public constructor(i: int) Id := i; end; end Pt;
extension Array<T> where T: class
  public method CSwap()
    var t: T := this[0]; this[0] := this[1]; this[1] := t;
  end CSwap;
end;
extension Array<Array<T>>
  public method NSwap()
    var t: T := this[0][0]; this[0][0] := this[1][0]; this[1][0] := t;
  end NSwap;
end;
class Program
  public static method Main(): int
    using var p1 := new Pt(1); using var p2 := new Pt(2);
    var c: Array<Pt> := [p1, p2];
    var i := 0;
    while i < 201 do c.CSwap(); i := i + 1; end;
    Console.WriteLine(c[0].Id);
    var x: Array<string> := ["a", "b"]; var y: Array<string> := ["c", "d"];
    var xs: Array<Array<string>> := [x, y];
    i := 0;
    while i < 201 do xs.NSwap(); i := i + 1; end;
    Console.WriteLine(x[0]); Console.WriteLine(y[0]);
    var dx: Array<double> := [1.5]; var dy: Array<double> := [2.5];
    var ds: Array<Array<double>> := [dx, dy];
    i := 0;
    while i < 201 do ds.NSwap(); i := i + 1; end;
    Console.WriteLine(dx[0]); Console.WriteLine(dy[0]);
    return 0;
  end Main;
end Program;
