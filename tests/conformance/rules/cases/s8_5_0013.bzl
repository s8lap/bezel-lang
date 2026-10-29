//! case: s8_5_0013
//! doc: spec
//! section: 8.5
//! title: a class without ToString renders as its qualified name and a positional record as its field listing
//! rule: For classes the default is the type's qualified name ("M.Res" in module M); for positional records a representation of fields (spec 8.5, v0.350).
//! cites: 8.5
//! expect: run
//! output: M.Res
//!   Rec(x: 1)
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
using var r := new Res(); Console.WriteLine(r); var rr := new Rec(1); Console.WriteLine(rr.ToString());
end Main;  end P;
