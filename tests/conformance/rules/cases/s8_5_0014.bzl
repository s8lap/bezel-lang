//! case: s8_5_0014
//! doc: spec
//! section: 8.5
//! title: WriteLine of a [2]string through a TextWriter local renders the type name
//! rule: For inline arrays (all forms, including move-only `[N owned]…` arrays, whose receiver is borrowed for the call) and for the unit type: the type's name — `"[3]int"`-style for an inline array
//! cites: 8.6, 7.4
//! expect: run
//! output: [2]string
//!   [2]string
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
 var a: [2]string := ["x" + 1.ToString(), "y"];
 for var i in 0..<2 do
 w.WriteLine(a);
 end for;
 end Main; end P;
