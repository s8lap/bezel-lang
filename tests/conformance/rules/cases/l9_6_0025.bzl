//! case: l9_6_0025
//! doc: stdlib
//! section: 9.6
//! title: string.Repeat with a negative count raises ArgumentOutOfRangeException
//! rule: public static pure method Repeat(c: char, count: int): string;  // count < 0
//!   raises ArgumentOutOfRangeException; count = 0 is ""
//! cites: 9.6
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := string.Repeat('x', -1);
    catch
      var ex: ArgumentOutOfRangeException => Console.WriteLine("caught");
    end try;
  end Main;
end Program;
