//! case: s35_8_0010
//! doc: spec
//! section: 35.8
//! title: an instantiation of a generic choice with an owning payload cannot be boxed (per instantiation)
//! rule: Owning choices cannot be boxed (spec 35.8, v0.350): boxing copies, and a move-only value cannot be copied. The conversion of an owning value to an interface or Object type is rejected at the conversion site. Console.WriteLine, Console.Write and TextWriter members take their argument as Object, so passing an owning choice there is such a conversion. For a generic choice the rule is per instantiation (spec 35.8, 3.6 rule 2): Rs<Res, string> is owning.
//! cites: 35.8, 8.6
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
choice Rs<T,E> Ok(owned v: T), Er(owned e: E); end Rs;
class P public static method Main()
var r := Rs<Res,string>.Ok(new Res()); Console.WriteLine(r);
end Main;  end P;
