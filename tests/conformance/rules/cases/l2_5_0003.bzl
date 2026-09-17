//! case: l2_5_0003
//! doc: stdlib
//! section: 2.5
//! title: Parse accepts the date-only form with zero time components
//! rule: DateTime.Parse accepts the date-only form yyyy-MM-dd, with time
//!   components zero.
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var d := DateTime.Parse("2024-03-15");
    Console.WriteLine(d.Hour);
  end Main;
end Program;
