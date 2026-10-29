//! case: s35_8_0012
//! doc: spec
//! section: 35.8
//! title: an owning choice with a declared ToString renders through it; a match reads the other case
//! rule: An owning choice cannot be boxed, but a ToString() call at its own static type is not a boxing: a declared ToString member is called in place on the borrowed storage (spec 35.8 and 8.5, v0.350).
//! cites: 35.8, 8.5, 35.2
//! expect: run
//! output: oc
//!   e
module M;
class Res public constructor() end constructor; end Res;
choice OC Text(owned r: Res), Empty; public override method ToString(): string => "oc"; end OC;
choice OC2 Text(owned r: Res), Empty; end OC2;
choice VC A(x: int), B; end VC;
record Rec(x: int);
class Cls public constructor() end constructor; public override method ToString(): string => "cls"; end Cls;
class Bx public owned V: OC2 := OC2.Empty; public owned W: OC := OC.Empty; end Bx;

class P public static method Main()
var bx := new Bx(); Console.WriteLine(bx.W.ToString()); case bx.V of Text(var r) => Console.WriteLine("t"), Empty => Console.WriteLine("e") end;
end Main; end P;
