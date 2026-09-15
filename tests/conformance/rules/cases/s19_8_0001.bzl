//! case: s19_8_0001
//! doc: spec
//! section: 19.8
//! title: an enum-typed scrutinee omits the enum prefix in patterns
//! rule: When the scrutinee has an enum type, the enum prefix may be
//!   omitted in patterns.
//! cites: 19.8, 10.6
//! expect: run
//! stdout: high
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    var p := Priority.High;
    case p of
      Low    => Console.WriteLine("low");,
      Medium => Console.WriteLine("medium");,
      High   => Console.WriteLine("high");
    end case;
  end Main;
end Program;
