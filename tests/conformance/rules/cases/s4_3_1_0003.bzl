//! case: s4_3_1_0003
//! doc: spec
//! section: 4.3.1
//! title: equality on a decimal is plain value equality, so 1.50m equals 1.5m
//! rule: Language spec §4.3.1 (v0.345): "`decimal` has no `Infinity`, `NaN`, or signed zero, so
//!   the question does not arise: `=` on `decimal` is straightforward value equality."
//! cites: 4.3.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(1.50m = 1.5m);
  end Main;
end Program;
