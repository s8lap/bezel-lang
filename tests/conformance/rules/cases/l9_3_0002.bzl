//! case: l9_3_0002
//! doc: stdlib
//! section: 9.3
//! title: Decimal.Round raises ArgumentOutOfRangeException outside 0..28 digits
//! rule: Round(digits: int): decimal; digits outside 0..28:
//!   ArgumentOutOfRangeException.
//! cites: 9.3
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var x := 1.5m;
    try
      _ := x.Round(29);
    catch
      ArgumentOutOfRangeException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
