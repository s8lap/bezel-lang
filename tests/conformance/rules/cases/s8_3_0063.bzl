//! case: s8_3_0063
//! doc: spec
//! section: 8.3
//! title: a second-parameter class-bounded choice with no owned payload obliges GetHashCode
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3, 35.7
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
choice W<X,T> where T: class T1(x: int), No;
  public method Equals(o: W<X,T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
