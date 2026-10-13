//! case: s8_3_0046
//! doc: spec
//! section: 8.3
//! title: an override GetHashCode discharges a record's plain own-typed Equals
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: run
//! output: 1
//!   7
//! issue: 5955
module Conf;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: R): bool => V = o.V;
  public override method GetHashCode(): int => V;
end R;
class Program public static method Main(): int
  var a := new R(7); var b := new R(7);
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
