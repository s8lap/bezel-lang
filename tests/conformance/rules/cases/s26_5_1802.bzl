//! case: s26_5_1802
//! doc: spec
//! section: 26.5
//! title: an extension member on IEnumerable<int> is reached on a FrozenArray<int> receiver
//! rule: for extension blocks on `x`'s type, base types, or implemented interfaces
//! cites: 4.19
//! expect: run
//! output: 6
//!   6
//! issue: 8103
module Conf;

extension IEnumerable<int>
  public method Total(): int
    var s := 0;
    for var x in this do s := s + x; end;
    return s;
  end Total;
end;
class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var f := FrozenArray<int>.From(a);
    using var g: FrozenArray<int> := FrozenArray<int>.From(a);
    Console.WriteLine(f.Total());
    Console.WriteLine(g.Total());
  end Main;
end Program;
