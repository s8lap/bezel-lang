//! case: s28_9_8008
//! doc: spec
//! section: 28.9
//! title: a nested record's function component stored through a record parameter sums the
//!   closure results
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 9.1, 27.5
//! expect: run
//! stdout: 600
//! notes: Pins #8964's fix-forward: the inner record's component stored via a parameter must
//!   keep its cell alive.
module Conf;
record In(f: () -> int);
record Out(i: In, n: int);
public class Program
  public static method Pull(i: In, fs: Array<() -> int>): int
    fs.Add(i.f);
    return 0;
  end Pull;
  public static method Main(): int
    var acc := 0;
    var fs := new Array<() -> int>();
    for var k in 0..<200 do
      var o := new Out(new In(lambda() => 3), 1);
      acc := acc + Pull(o.i, fs);
    end;
    for var g in fs do acc := acc + g(); end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
