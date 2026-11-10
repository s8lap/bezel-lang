//! case: s5_1_4205
//! doc: spec
//! section: 5.1
//! title: an unannotated FrozenArray<int> converts to the IEnumerable<int> it declares
//! rule: a type to an interface it implements or conforms to
//! cites: 5.1, stdlib 4.19
//! expect: run
//! output: 3
module Conf;

public class Program
  static method N(x: IEnumerable<int>): int
    var n := 0;
    for var v in x do
      n := n + 1;
    end;
    return n;
  end N;

  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    using var g := FrozenArray<int>.From(a);
    Console.WriteLine(N(g));
    return 0;
  end Main;
end Program;
