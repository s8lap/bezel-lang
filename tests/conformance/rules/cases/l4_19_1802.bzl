//! case: l4_19_1802
//! doc: stdlib
//! section: 4.19
//! title: ToArray on a FrozenArray<int> parameter returns a fresh mutable copy
//! rule: public method ToArray(): owned Array<T>;
//! cites: 26.5
//! expect: run
//! output: 13
//! issue: 8104
module Conf;

class Program
  static method N(xs: FrozenArray<int>): int
    using var b := xs.ToArray();
    return b.Count + b[0];
  end N;
  public static method Main()
    var a: Array<int> := [10, 2, 3];
    using var f := FrozenArray<int>.From(a);
    Console.WriteLine(N(f));
  end Main;
end Program;
