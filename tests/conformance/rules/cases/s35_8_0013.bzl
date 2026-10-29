//! case: s35_8_0013
//! doc: spec
//! section: 35.8
//! title: a generic function over an unconstrained T prints a value-like instantiation (int) through Object
//! rule: Boxing is per instantiation (spec 35.8, 3.6 rule 2, v0.350): at T = int the value-like int boxes normally and prints.
//! cites: 35.8, 8.6
//! expect: run
//! output: 1
//!   x
module M;
class Res public constructor() end constructor; end Res;
choice OC Text(owned r: Res), Empty; public override method ToString(): string => "oc"; end OC;
choice OC2 Text(owned r: Res), Empty; end OC2;
choice VC A(x: int), B; end VC;
record Rec(x: int);
class Cls public constructor() end constructor; public override method ToString(): string => "cls"; end Cls;
class Bx public owned V: OC2 := OC2.Empty; public owned W: OC := OC.Empty; end Bx;
class P public static method Main()
Console.WriteLine(P.G<int>(1));
end Main; public static method G<T>(owned t: T): string Console.WriteLine(t); return "x"; end G; end P;
