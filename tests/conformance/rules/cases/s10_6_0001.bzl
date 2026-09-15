//! case: s10_6_0001
//! doc: spec
//! section: 10.6
//! title: case matching an enum scrutinee admits the omitted prefix and selects the matching arm
//! rule: Inside a `case` whose scrutinee is an enum, the enum prefix may be
//!   omitted.
//! cites: 10.6
//! expect: run
//! stdout: medium or high
module Conf;

enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    var p := Priority.High;
    case p of
      Low => Console.WriteLine("low"),
      Medium | High => Console.WriteLine("medium or high")
    end;
  end Main;
end Program;
