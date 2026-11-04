//! case: s9_3_0009
//! doc: spec
//! section: 9.3
//! title: a positional record's defaultable body field needs no initializer beside a body constructor
//! rule: satisfies the must-initialize rule vacuously and is implicitly initialized to that default
//! cites: 9.3, 7.9
//! expect: run
//! issue: 8097
//! output: 1
//!   9
module Conf;

public record Holder(Seed: int)
  private _n: int;

  public constructor(seed: int, k: int)
    Seed := seed;
    _n := k;
  end constructor;

  public method N(): int => _n;
end Holder;

public class Program
  public static method Main()
    var h := new Holder(1, 9);
    Console.WriteLine(h.Seed);
    Console.WriteLine(h.N());
  end Main;
end Program;
