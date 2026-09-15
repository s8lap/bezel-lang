//! case: s10_1_0001
//! doc: spec
//! section: 10.1
//! title: default underlying values are assigned 0, 1, 2 in declaration order
//! rule: This declares constants `Priority.Low`, `Priority.Medium`,
//!   `Priority.High` of type `Priority`. The default underlying values are
//!   0, 1, 2 — assigned in declaration order from zero.
//! cites: 10.1
//! expect: run
//! stdout: 1
module Conf;

enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    Console.WriteLine(int(Priority.Medium));
  end Main;
end Program;
