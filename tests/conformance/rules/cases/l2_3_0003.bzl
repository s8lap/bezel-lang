//! case: l2_3_0003
//! doc: stdlib
//! section: 2.3
//! title: ToString(value, toBase) rejects a base outside {2, 8, 10, 16}
//! rule: Convert.ToString(value, toBase) accepts only bases 2, 8, 10, and 16,
//!   raising ArgumentException otherwise.
//! expect: run
//! stdout: threw
//! issue: 7268
module Conf;

public class Program
  public static method Main()
    try
      _ := Convert.ToString(42, 3);
      Console.WriteLine("no exception");
    catch
      ArgumentException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
