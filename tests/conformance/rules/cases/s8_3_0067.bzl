//! case: s8_3_0067
//! doc: spec
//! section: 8.3
//! title: an interface-bounded owned-payload choice is not owning at every instantiation
//! rule: A choice that no boxing reaches, one owning at every
//!   instantiation, has no slot to fill, so its declared `Equals` carries
//!   no such obligation (v0.338, §8.3).
//! cites: 8.3, 35.4, 35.11
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
interface IShape end IShape;
choice W<T> where T: IShape Ok(owned v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
