//! case: s10_4_0001
//! doc: spec
//! section: 10.4
//! title: FromUnderlyingOrNull returns the declared constant matching the value
//! rule: `FromUnderlyingOrNull` returns the declared constant whose
//!   underlying value equals `value`, or `null` when there is none.
//! cites: 10.4
//! expect: run
//! stdout: True
module Conf;

enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    var p := Priority.FromUnderlyingOrNull(1);
    Console.WriteLine(p = Priority.Medium);
  end Main;
end Program;
