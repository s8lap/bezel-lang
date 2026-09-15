//! case: s25_0001
//! doc: spec
//! section: 25
//! title: an enum may overload an operator
//! rule: Records, enums, choices, and classes may overload operators; an enum
//!   body admits operator overloads among the members it may declare (§10.4),
//!   and public static is the required spelling.
//! cites: 25.1, 10.4
//! expect: run
//! stdout: 2
module Conf;

public enum Priority
  Low := 1, Medium := 2, High := 3;

  public static method operator -(a: Priority, b: Priority): int =>
    int(a) - int(b);
end Priority;

public class Program
  public static method Main()
    Console.WriteLine(Priority.High - Priority.Low);
  end Main;
end Program;
