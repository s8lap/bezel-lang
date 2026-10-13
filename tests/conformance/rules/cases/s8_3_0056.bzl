//! case: s8_3_0056
//! doc: spec
//! section: 8.3
//! title: a hashable-constrained owning choice obliges nothing
//! rule: A choice that no boxing reaches, one owning at every
//!   instantiation, has no slot to fill, so its declared `Equals` carries
//!   no such obligation (v0.338, §8.3).
//! cites: 8.3, 35.4, 14.2
//! expect: run
//! output: run
//! issue: 5955
module Conf;
class R public V: int := 0; destructor end destructor; end R;
choice W<T> where T: hashable Ok(owned v: R, w: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  var a := W<int>.No;
  Console.WriteLine("run");
  return 0;
end Main; end Program;
