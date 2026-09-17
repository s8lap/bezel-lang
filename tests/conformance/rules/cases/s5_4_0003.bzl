//! case: s5_4_0003
//! doc: spec
//! section: 5.4
//! title: an int-to-enum cast of a non-constant undeclared value raises OverflowException
//! rule: When n is not a constant, a value equal to no declared constant
//!   raises OverflowException at runtime, the diagnostic naming the enum type
//!   and the value.
//! cites: 5.4
//! expect: run
//! stdout: overflow
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Make(n: int): Priority => Priority(n);

  public static method Main()
    try
      var p := Make(5);
      Console.WriteLine(Priority.NameOf(p));
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
