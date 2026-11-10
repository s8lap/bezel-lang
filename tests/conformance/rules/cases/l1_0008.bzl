//! case: l1_0008
//! doc: stdlib
//! section: 1
//! title: First() on an empty collection passes an IFailure arm
//! rule: It comes from the program itself when it is a fact about what the program
//!   passed (an index, a key, a count, a radix, a format specifier), about the
//!   state of an object or collection the program holds (an empty collection,
//!   an absent key, a closed scope, a live enumeration), or about its own
//!   arithmetic.
//! cites: 1, 5
//! expect: run
//! stdout: defect passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    try
      try
        _ := xs.First();
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      InvalidOperationException => Console.WriteLine("defect passed")
    end;
  end Main;
end Program;
