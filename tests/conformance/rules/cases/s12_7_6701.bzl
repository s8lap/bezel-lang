//! case: s12_7_6701
//! doc: spec
//! section: 12.7
//! title: an is test narrows an interface-typed local to the class so the call is admitted
//! rule: the carrier is **flow-narrowed** to `T` within the controlled block
//! cites: 5.1, 7.4
//! expect: run
//! stdout: not an exception
module Conf;

public interface IMark
end IMark;

public class NotAnException: IMark
end NotAnException;

public class Program
  static method Log(ex: Exception)
    Console.WriteLine($"Log: {ex.Message}");
  end Log;
  public static method Main()
    using var n := new NotAnException();
    var j: IMark := n;
    if j is Exception then
      Program.Log(j);
    else
      Console.WriteLine("not an exception");
    end if;
  end Main;
end Program;
