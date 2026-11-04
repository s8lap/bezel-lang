//! case: l5_1_0011
//! doc: stdlib
//! section: 5.1
//! title: Min() on an empty Array<DateTime> raises InvalidOperationException
//! rule: InvalidOperationException if empty
//! cites: 5.1, 2.5
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<DateTime>();
    try
      var m := xs.Min();
      Console.WriteLine("no exception");
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
