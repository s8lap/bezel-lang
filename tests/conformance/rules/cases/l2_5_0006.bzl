//! case: l2_5_0006
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan subtraction computes the elapsed duration
//! rule: TimeSpan declares operator -(a, b): TimeSpan; subtracting one hour
//!   from one day leaves a TimeSpan whose TotalHours is 23.
//! expect: run
//! stdout: 23
module Conf;

public class Program
  public static method Main()
    var remaining := TimeSpan.FromDays(1) - TimeSpan.FromHours(1);
    Console.WriteLine(remaining.TotalHours);
  end Main;
end Program;
