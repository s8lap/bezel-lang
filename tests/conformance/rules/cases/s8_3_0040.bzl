//! case: s8_3_0040
//! doc: spec
//! section: 8.3
//! title: a plain GetHashCode discharges an enum's plain own-typed Equals
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: run
//! output: ok
//! issue: 5955
module Conf;
enum E A, B;
  public method Equals(o: E): bool => int(this) = int(o);
  public method GetHashCode(): int => int(this);
end E;
class Program public static method Main(): int Console.WriteLine("ok"); return 0; end Main; end Program;
