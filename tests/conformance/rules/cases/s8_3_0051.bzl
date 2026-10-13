//! case: s8_3_0051
//! doc: spec
//! section: 8.3
//! title: a generic owning choice's plain own-typed Equals obliges nothing
//! rule: A choice that no boxing reaches, one owning at every
//!   instantiation, has no slot to fill, so its declared `Equals` carries
//!   no such obligation (v0.338, §8.3).
//! cites: 8.3, 35.4
//! expect: run
//! output: run
//! issue: 5955
module Conf;
class R public V: int := 0; destructor end destructor; end R;
choice Res<T> Ok(owned v: R), No;
  public method Equals(o: Res<T>): bool => true;
end Res;
class Program public static method Main(): int
  var a := Res<R>.No;
  Console.WriteLine("run");
  return 0;
end Main; end Program;
