//! case: s22_5_0008
//! doc: spec
//! section: 22.5
//! title: supplied arguments evaluate in call-site textual order regardless of naming
//! rule: "A call's **supplied** argument expressions are evaluated in
//!   **call-site textual order** — left to right as written, for positional
//!   and named arguments alike (`M(b := Next(), a := Next())` evaluates the
//!   `b` expression first)."
//! cites: 22.5
//! expect: run
//! stdout: B
module Conf;

public class Tracker
  public static method Next(tag: string): int
    Console.WriteLine(tag);
    return 0;
  end Next;

  public static method Combine(b: int, a: int): int => a + b;
end Tracker;

public class Program
  public static method Main()
    Tracker.Combine(b := Tracker.Next("B"), a := Tracker.Next("A"));
  end Main;
end Program;
