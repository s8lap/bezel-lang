//! case: s26_4_0016
//! doc: spec
//! section: 26.4
//! title: block-level T stays bound per receiver when an outer generic method or class has its own T
//! rule: The block-level type parameter is in scope throughout the block. ... Each member of a generic extension block is read ... as a generic method that declares the block's parameters before its own — bound by the receiver at a call or a property read (§26.5 step 2)
//! cites: 26.5, 14.1
//! expect: run
//! output: b
//!   2
//!   d
//!   8
//!   6
//!   f
//!   q
//!   10
//!   s
//!   4.5
//!   40
//!   2
//!   n
//!   2.5
//!   v
//!   12
//!   x
module Conf;
extension Array<T>
  public method ASwap()
    var t: T := this[0]; this[0] := this[1]; this[1] := t;
  end ASwap;
end;
class U
  // outer generic method; receiver a is Array<T>, b is a DIFFERENT element type
  public static method MixIS<T>(a: Array<T>, b: Array<int>, c: Array<string>)
    var i := 0;
    while i < 201 do a.ASwap(); b.ASwap(); c.ASwap(); i := i + 1; end;
  end MixIS;
end U;
class G<T>
  public _a: Array<T>;
  public constructor(a: Array<T>) _a := a; end constructor;
  public method Go(b: Array<int>, c: Array<string>, d: Array<double>)
    var i := 0;
    while i < 201 do _a.ASwap(); b.ASwap(); c.ASwap(); d.ASwap(); i := i + 1; end;
  end Go;
  public method Lam(b: Array<int>, c: Array<string>)
    var f := lambda() => do b.ASwap(); c.ASwap(); _a.ASwap(); end;
    var i := 0;
    while i < 201 do f(); i := i + 1; end;
  end Lam;
end G;
class Program
  public static method Main(): int
    var s1: Array<string> := ["a", "b"]; var i1: Array<int> := [1, 2]; var s2: Array<string> := ["c", "d"];
    U.MixIS<string>(s1, i1, s2);
    Console.WriteLine(s1[0]); Console.WriteLine(i1[0]); Console.WriteLine(s2[0]);
    var d1: Array<double> := [1.5, 2.5]; var i2: Array<int> := [5, 6]; var s3: Array<string> := ["e", "f"];
    var i3: Array<int> := [7, 8];
    U.MixIS<int>(i3, i2, s3);
    Console.WriteLine(i3[0]); Console.WriteLine(i2[0]); Console.WriteLine(s3[0]);
    var s4: Array<string> := ["p", "q"]; var i4: Array<int> := [9, 10]; var s5: Array<string> := ["r","s"];
    var d4: Array<double> := [3.5, 4.5];
    var g := new G<string>(s4);
    g.Go(i4, s5, d4);
    Console.WriteLine(s4[0]); Console.WriteLine(i4[0]); Console.WriteLine(s5[0]); Console.WriteLine(d4[0]);
    var i6: Array<int> := [1, 2]; var s6: Array<string> := ["m", "n"]; var i7: Array<int> := [30, 40];
    var g2 := new G<int>(i7);
    g2.Go(i6, s6, d1);
    Console.WriteLine(i7[0]); Console.WriteLine(i6[0]); Console.WriteLine(s6[0]); Console.WriteLine(d1[0]);
    var s8: Array<string> := ["u", "v"]; var i8: Array<int> := [11, 12]; var s9: Array<string> := ["w","x"];
    var g3 := new G<string>(s8);
    g3.Lam(i8, s9);
    Console.WriteLine(s8[0]); Console.WriteLine(i8[0]); Console.WriteLine(s9[0]);
    return 0;
  end Main;
end Program;
