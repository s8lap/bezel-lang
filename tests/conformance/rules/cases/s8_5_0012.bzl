//! case: s8_5_0012
//! doc: spec
//! section: 8.5
//! title: boxing of a class, record, string, int, nullable int and value-like choice to Console members ...
//! rule: Value-like auto-hashable choices, classes, records and integers box and render normally (spec 8.5, 35.8, v0.350): a class renders by its declared ToString, a positional record as Rec(x: 3), a value-like choice as TypeName.CaseName; 
//! cites: 8.5, 8.6, 35.8
//! expect: run
//! output: cls
//!   Rec(x: 3)
//!   s
//!   5
//!   4
//!   VC.B
//!   cls
module M;
class Res public constructor() end constructor; end Res;
choice OC Text(owned r: Res), Empty; public override method ToString(): string => "oc"; end OC;
choice OC2 Text(owned r: Res), Empty; end OC2;
choice VC A(x: int), B; end VC;
record Rec(x: int);
class Cls public constructor() end constructor; public override method ToString(): string => "cls"; end Cls;
class Bx public owned V: OC2 := OC2.Empty; public owned W: OC := OC.Empty; end Bx;

class P public static method Main()
Console.WriteLine(new Cls()); Console.WriteLine(new Rec(3)); Console.WriteLine("s"); Console.WriteLine(5); var n: int? := 4; if n <> null then Console.WriteLine(n); end; Console.Out.WriteLine(VC.B); Console.Write(new Cls());
end Main; end P;
