//! case: s9_3_0010
//! doc: spec
//! section: 9.3
//! title: a positional record's nullable body field needs no initializer beside a body constructor
//! rule: satisfies the must-initialize rule vacuously and is implicitly initialized to that default
//! cites: 9.3, 7.9
//! expect: run
//! issue: 8097
//! output: 1
//!   q
module Conf;

public record Holder(Seed: int)
  private _c: string?;

  public constructor(seed: int, c: string)
    Seed := seed;
    _c := c;
  end constructor;

  public method C(): string => _c ?? "none";
end Holder;

public class Program
  public static method Main()
    var h := new Holder(1, "q");
    Console.WriteLine(h.Seed);
    Console.WriteLine(h.C());
  end Main;
end Program;
