//! case: s26_5_1805
//! doc: spec
//! section: 26.5
//! title: an IEnumerable<int> extension is reached on a FrozenArray<int> parameter
//! rule: for extension blocks on `x`'s type, base types, or implemented interfaces
//! cites: 4.19
//! expect: run
//! output: 7
//! issue: 8103
module Conf;

extension IEnumerable<int>
  public method Foo(): int => 7;
end;
class Program
  static method N(xs: FrozenArray<int>): int => xs.Foo();
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var f := FrozenArray<int>.From(a);
    Console.WriteLine(N(f));
  end Main;
end Program;
