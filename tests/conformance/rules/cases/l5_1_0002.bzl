//! case: l5_1_0002
//! doc: stdlib
//! section: 5.1
//! title: Min() on an empty source raises InvalidOperationException
//! rule: Min and Max without a selector are generic over comparable types;
//!   Min()/Max() -- selector and no-selector forms alike -- throw
//!   InvalidOperationException on an empty source.
//! cites: 5.1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    try
      var m := xs.Min();
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
