//! case: s8_3_0049
//! doc: spec
//! section: 8.3
//! title: a generic record's own-typed Equals with GetHashCode runs
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: run
//! output: 1
//!   3
//! issue: 5955
module Conf;
record G<T> public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: G<T>): bool => V = o.V;
  public method GetHashCode(): int => V;
end G;
class Program public static method Main(): int
  var a := new G<int>(3); var b := new G<int>(3);
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
