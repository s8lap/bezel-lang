//! case: s8_3_0060
//! doc: spec
//! section: 8.3
//! title: a notnull payload choice owned only at some instantiations keeps the obligation
//! rule: A choice that no boxing reaches, one owning at every
//!   instantiation, has no slot to fill, so its declared `Equals` carries
//!   no such obligation (v0.338, §8.3).
//! cites: 8.3, 35.4, 35.11
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
choice Wrap<T> where T: notnull Has(owned v: T), No;
  public method Equals(o: Wrap<T>): bool => true;
end Wrap;
class Program public static method Main(): int
  Console.WriteLine("run");
  return 0;
end Main; end Program;
