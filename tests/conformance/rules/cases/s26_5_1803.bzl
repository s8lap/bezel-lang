//! case: s26_5_1803
//! doc: spec
//! section: 26.5
//! title: an extension on IEnumerable<T> infers T from a FrozenArray<int> receiver
//! rule: the block-level type parameter is inferred from the receiver's type
//! cites: 26.4, 4.19
//! expect: run
//! output: 4
//!   4
//! issue: 8103
module Conf;

extension IEnumerable<T>
  public method Tally(): int
    var n := 0;
    for var x in this do n := n + 1; end;
    return n;
  end Tally;
end;
class Program
  public static method Main()
    var a: Array<int> := [4, 5, 6, 7];
    using var f := FrozenArray<int>.From(a);
    using var g: FrozenArray<int> := FrozenArray<int>.From(a);
    Console.WriteLine(f.Tally());
    Console.WriteLine(g.Tally());
  end Main;
end Program;
