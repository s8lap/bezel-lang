//! case: s35_8_0011
//! doc: spec
//! section: 35.8
//! title: a nullable owning choice boxes as its non-nullable form and is rejected
//! rule: Owning choices cannot be boxed (spec 35.8, v0.350): boxing copies, and a move-only value cannot be copied. The conversion of an owning value to an interface or Object type is rejected at the conversion site. Console.WriteLine, Console.Write and TextWriter members take their argument as Object, so passing an owning choice there is such a conversion. A nullable value type boxes as its non-nullable form (spec 8.6 Nullable value-type boxing), so OC2? is the owning choice OC2 at the conversion.
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
var x: OC2? := OC2.Empty; Console.WriteLine(x);
end Main;  end P;
