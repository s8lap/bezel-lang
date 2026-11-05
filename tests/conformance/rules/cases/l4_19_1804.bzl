//! case: l4_19_1804
//! doc: stdlib
//! section: 4.19
//! title: Length and the indexer on an annotated FrozenArray<int> read the frozen elements
//! rule: public readonly property Length: int;
//! cites: 26.5
//! expect: run
//! output: 3
//!   2
module Conf;

class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var g: FrozenArray<int> := FrozenArray<int>.From(a);
    Console.WriteLine(g.Length);
    Console.WriteLine(g[1]);
  end Main;
end Program;
