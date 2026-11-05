//! case: s26_5_1807
//! doc: spec
//! section: 26.5
//! title: an IReadOnlyCollection<int> extension is not reached on an inferred FrozenArray<int> receiver
//! rule: Where neither rule 1 nor this search supplies the name, nothing supplies
//!   it: the access names no member and is a compile error at the name, the
//!   diagnostic naming the receiver's static type
//! cites: 4.19
//! expect: reject
//! diagnostic: no member
module Conf;

extension IReadOnlyCollection<int>
  public method Foo(): int => 7;
end;
class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var f := FrozenArray<int>.From(a);
    Console.WriteLine(f.Foo());
  end Main;
end Program;
