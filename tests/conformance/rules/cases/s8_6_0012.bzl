//! case: s8_6_0012
//! doc: spec
//! section: 8.6
//! title: an owning inline array cannot be boxed to Object
//! rule: Boxing copies, and a move-only value type (an owning inline array) cannot be copied, so it cannot be boxed (spec 8.6 and 15.3, v0.350); passing it where Object is expected, as Console.WriteLine does, is rejected.
//! cites: 8.6, 15.3
//! expect: reject
//! diagnostic: box
module M;
class Res public constructor() end constructor; end Res;
choice OC Text(owned r: Res), Empty; public override method ToString(): string => "oc"; end OC;
choice OC2 Text(owned r: Res), Empty; end OC2;
choice VC A(x: int), B; end VC;
record Rec(x: int);
class Cls public constructor() end constructor; public override method ToString(): string => "cls"; end Cls;
class Bx public owned V: OC2 := OC2.Empty; public owned W: OC := OC.Empty; end Bx;

class P public static method Main()
var a: [2 owned]Res := [new Res(), new Res()]; Console.WriteLine(a);
end Main; end P;
