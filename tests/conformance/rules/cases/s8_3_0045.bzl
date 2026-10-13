//! case: s8_3_0045
//! doc: spec
//! section: 8.3
//! title: an override GetHashCode discharges a choice's plain own-typed Equals
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: run
//! output: 1
//!   4
//! issue: 5955
module Conf;
choice K One, Two;
  public method Equals(o: K): bool => true;
  public override method GetHashCode(): int => 4;
end K;
class Program public static method Main(): int
  var a := K.One; var b := K.Two;
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
