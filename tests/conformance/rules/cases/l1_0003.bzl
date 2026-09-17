//! case: l1_0003
//! doc: stdlib
//! section: 1
//! title: a negative constructor magnitude raises ArgumentOutOfRangeException
//! rule: Every constructor parameter that states a magnitude -- capacity, size,
//!   length -- raises ArgumentOutOfRangeException when passed a negative value;
//!   StringBuilder(capacity) is one of the named members.
//! cites: 2.1
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      var sb := new StringBuilder(-1);
      Console.WriteLine("no exception");
    catch
      ArgumentOutOfRangeException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
