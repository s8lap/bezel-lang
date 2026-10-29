//! case: s8_5_0016
//! doc: spec
//! section: 8.5
//! title: a nullable-element inline array renders [3]int? through ToString and as an Object
//! rule: For inline arrays (all forms, including move-only `[N owned]…` arrays, whose receiver is borrowed for the call) and for the unit type: the type's name — `"[3]int"`-style for an inline array
//! cites: 8.6
//! expect: run
//! output: [3]int?
//!   [3]int?
//!   [3]int?
//!   [3]int?
module Conf;
import System.IO;
class Cls public constructor() end constructor; public override method ToString(): string => "cls"; end Cls;
class Res public constructor() end constructor; destructor end destructor; public override method ToString(): string => "res"; end Res;
record Rec(x: int);
enum Color Red, Green; end Color;
choice VC A(x: int), B; end VC;
class P
 static method Mk(i: int): string => "mk" + i.ToString();
 static method MkObj(i: int): Object => i;
 public static method Main()
 var w: TextWriter := Console.Out;
 var a: [3]int? := [1, null, 3];
 for var i in 0..<1 do
 w.WriteLine(a); w.WriteLine(a.ToString()); var o: Object := a; w.WriteLine(o); w.WriteLine(o.ToString());
 end for;
 end Main; end P;
