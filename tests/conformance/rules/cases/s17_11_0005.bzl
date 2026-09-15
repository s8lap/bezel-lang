//! case: s17_11_0005
//! doc: spec
//! section: 17.11
//! title: default of an enum with a zero-valued constant is that constant
//! rule: "Enums: the value with underlying integer 0 — necessarily a
//!   declared constant, per the compile error below."
//! cites: 17.11
//! expect: run
//! stdout: True
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    var p := default(Priority);
    Console.WriteLine(p = Priority.Low);
  end Main;
end Program;
