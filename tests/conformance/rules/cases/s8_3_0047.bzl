//! case: s8_3_0047
//! doc: spec
//! section: 8.3
//! title: an override GetHashCode discharges an enum's plain own-typed Equals
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: run
//! output: 0
//!   10
//! issue: 5955
module Conf;
enum E A, B;
  public method Equals(o: E): bool => int(this) = int(o);
  public override method GetHashCode(): int => int(this) + 10;
end E;
class Program public static method Main(): int
  var a := E.A; var b := E.B;
  Console.WriteLine((if a.Equals(b) then 1 else 0).ToString());
  Console.WriteLine(a.GetHashCode().ToString());
  return 0;
end Main; end Program;
