//! case: s35_8_0016
//! doc: spec
//! section: 35.8
//! title: a payload-free case of an owning choice (declared owned Array<int> payload) is not boxable either
//! rule: Owning** choices **cannot be boxed**
//! cites: 35.8, 8.6
//! expect: reject
//! diagnostic: box
module Conf;

class Res public constructor() end constructor; end Res;
choice OC Text(owned r: Res), Empty; public override method ToString(): string => "oc"; end OC;
choice OC2 Text(owned r: Res), Empty; end OC2;
choice AC Arr(owned a: Array<int>), E; end AC;
choice VC A(x: int), B; end VC;
choice SC S(s: string), E; end SC;
choice Opt<T> Some(v: T), None; end Opt;
class Bx public owned V: OC2 := OC2.Empty; public owned W: OC := OC.Empty; public C: VC := VC.A(3); end Bx;
class Holder public owned sb: StringBuilder := new StringBuilder(); end Holder;

class P public static method Main()
var sb := new StringBuilder(); sb.Append(AC.E);
end Main; end P;
