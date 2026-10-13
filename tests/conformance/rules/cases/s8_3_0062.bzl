//! case: s8_3_0062
//! doc: spec
//! section: 8.3
//! title: an interface-bounded choice with no owned payload obliges GetHashCode
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3, 35.7
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
interface IShape end IShape;
choice W<X,T> where X: IShape X1(x: int), No;
  public method Equals(o: W<X,T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
