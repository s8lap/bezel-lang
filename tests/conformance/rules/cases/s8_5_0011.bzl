//! case: s8_5_0011
//! doc: spec
//! section: 8.5
//! title: ToString() on an owning choice with no declared ToString is a use-site compile error
//! rule: For a choice that is owning (no default is generated), a ToString() call at a statically-known receiver type is a use-site compile error, removed by declaring the member (spec 8.5, 35.7, v0.350).
//! cites: 8.5, 35.7
//! expect: reject
//! diagnostic: tostring
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
var x := OC2.Empty; Console.WriteLine(x.ToString());
end Main;  end P;
