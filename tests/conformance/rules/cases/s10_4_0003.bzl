//! case: s10_4_0003
//! doc: spec
//! section: 10.4
//! title: NameOf agrees with nameof for a written constant
//! rule: For a written constant `E.NameOf(E.A) = nameof(E.A)` — `NameOf`
//!   extends the compile-time operator to a runtime value over the same
//!   table.
//! cites: 10.4, 17.9
//! expect: run
//! stdout: True
module Conf;

enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    Console.WriteLine(Priority.NameOf(Priority.High) = nameof(Priority.High));
  end Main;
end Program;
