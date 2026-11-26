//! case: s29_2_3603
//! doc: spec
//! section: 29.2
//! title: a capture-free function value yielded from an iterator may be kept across advances
//! rule: a capture-free function value is yielded as a shared reference (the runtime-managed case above)
//! cites: 29.2, 28.3.8
//! expect: run
//! stdout: 7
//! issue: 9088
//! notes: Accepting twin of s29_2_3602: the lambda captures nothing.
module Conf;

public class Program
  public static method Gen(): IEnumerable<() -> int>
    for var i in 0..3 do
      yield return lambda(): int => 7;
    end;
  end Gen;
  public static method Main(): int
    var keep: () -> int := lambda(): int => 0;
    for var g in Gen() do
      keep := g;
    end;
    Console.WriteLine($"{keep()}");
    return 0;
  end Main;
end Program;
