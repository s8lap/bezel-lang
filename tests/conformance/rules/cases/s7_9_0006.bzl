//! case: s7_9_0006
//! doc: spec
//! section: 7.9
//! title: a defaultable local satisfies the must-initialize rule vacuously
//! rule: A field (or local) whose type is defaultable satisfies the
//!   must-initialize rule vacuously and is implicitly initialized to that
//!   default — `null` for a nullable local, `0` for a numeric local.
//! cites: 17.11
//! expect: run
//! stdout: 0|null
module Conf;

public class Program
  public static method Main()
    var count: int;
    var label: string?;
    Console.WriteLine($"{count}|{label}");
  end Main;
end Program;
