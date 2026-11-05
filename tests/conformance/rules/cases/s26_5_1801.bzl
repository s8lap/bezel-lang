//! case: s26_5_1801
//! doc: spec
//! section: 26.5
//! title: an extension on FrozenArray<int> outranks one on the IEnumerable<int> it implements
//! rule: Derived class beats base class; class beats interface implemented by that
//!   class; derived interface beats extended interface.
//! cites: 26.5.1, 4.19
//! expect: run
//! output: 2
//!   2
module Conf;

extension IEnumerable<int>
  public method Foo(): int => 1;
end;
extension FrozenArray<int>
  public method Foo(): int => 2;
end;
class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var f := FrozenArray<int>.From(a);
    using var g: FrozenArray<int> := FrozenArray<int>.From(a);
    Console.WriteLine(f.Foo());
    Console.WriteLine(g.Foo());
  end Main;
end Program;
