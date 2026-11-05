//! case: s26_5_1811
//! doc: spec
//! section: 26.5
//! title: an IEnumerable<int> extension is not reached on a user class named FrozenArray
//! rule: Where neither rule 1 nor this search supplies the name, nothing supplies
//!   it: the access names no member and is a compile error at the name, the
//!   diagnostic naming the receiver's static type
//! cites: 2.2
//! expect: reject
//! diagnostic: no member
module Conf;

class FrozenArray<T>
  public method Bar(): int => 41;
end FrozenArray;
extension IEnumerable<int>
  public method Foo(): int => 7;
end;
class Program
  public static method Main()
    using var g: FrozenArray<int> := new FrozenArray<int>();
    Console.WriteLine(g.Foo());
  end Main;
end Program;
