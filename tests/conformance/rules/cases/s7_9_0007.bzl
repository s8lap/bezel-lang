//! case: s7_9_0007
//! doc: spec
//! section: 7.9
//! title: a defaultable field with no initializer satisfies the rule vacuously
//! rule: A field whose type is defaultable satisfies the must-initialize rule
//!   vacuously and is implicitly initialized to that default; an explicit
//!   initializer is permitted and overrides it.
//! cites: 17.11
//! expect: run
//! stdout: 0
module Conf;

public class Counter
  private _n: int;

  public constructor()
  end constructor;

  public method Value(): int => _n;
end Counter;

public class Program
  public static method Main()
    using var c := new Counter();
    Console.WriteLine(c.Value());
  end Main;
end Program;
