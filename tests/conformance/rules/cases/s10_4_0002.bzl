//! case: s10_4_0002
//! doc: spec
//! section: 10.4
//! title: FromNameOrNull returns null for a digit string
//! rule: `FromNameOrNull(name)` returns the declared constant whose
//!   identifier equals `name`, or `null` when there is none; a digit string
//!   names no constant, since no identifier begins with a digit.
//! cites: 10.4, 1
//! expect: run
//! stdout: True
module Conf;

enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    var p := Priority.FromNameOrNull("1");
    Console.WriteLine(p = null);
  end Main;
end Program;
