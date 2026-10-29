//! case: s35_8_0018
//! doc: spec
//! section: 35.8
//! title: value-like choices box at StringBuilder.Append, plain, nullable, generic and string-payload alike
//! rule: **Value-like** choices box per the general rule; the box is runtime-managed.
//! cites: 35.8, 8.6
//! expect: run
//! output: True
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
  var bx := new Bx(); var sb := new StringBuilder();
  var n: VC? := VC.B;
  sb.Append(n);
  var o: Opt<int> := Opt<int>.Some(3);
  sb.Append(o);
  sb.Append(SC.S("hi"));
  sb.Append("a").Append(bx.C).Append(1);
  Console.WriteLine(sb.Length > 0);
end Main; end P;
