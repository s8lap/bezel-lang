//! case: l3_1_0003
//! doc: stdlib
//! section: 3.1
//! title: Replace with an empty oldValue raises ArgumentException
//! rule: Replace raises ArgumentException when oldValue is empty.
//! cites: 3.1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    try
      _ := sb.Replace("", "x");
    catch
      ArgumentException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
