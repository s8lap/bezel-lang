//! case: l3_1_0004
//! doc: stdlib
//! section: 3.1
//! title: a negative capacity raises ArgumentOutOfRangeException
//! rule: Negative magnitudes fault — StringBuilder(capacity) raises
//!   ArgumentOutOfRangeException when passed a negative value.
//! cites: 3.1, 1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      var sb := new StringBuilder(-1);
    catch
      ArgumentOutOfRangeException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
