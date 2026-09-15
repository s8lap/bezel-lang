//! case: s7_9_0009
//! doc: spec
//! section: 7.9
//! title: an enum field with a zero-valued constant is implicitly initialized to it
//! rule: A field whose type is defaultable satisfies the must-initialize rule
//!   vacuously and is implicitly initialized to that default; for an enum the
//!   default is the value with underlying integer 0.
//! cites: 17.11, 5.4
//! expect: run
//! stdout: 0
module Conf;

public enum Level
  None := 0, Low := 1, High := 2;
end Level;

public class Config
  private _level: Level;

  public constructor()
  end constructor;

  public method Raw(): int => int(_level);
end Config;

public class Program
  public static method Main()
    using var c := new Config();
    Console.WriteLine(c.Raw());
  end Main;
end Program;
