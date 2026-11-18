//! case: l9_2_0008
//! doc: stdlib
//! section: 9.2
//! title: an inadmissible input spelling a number raises FormatException, not OverflowException
//! rule: An input the grammar does not admit raises `FormatException` whatever
//!   number a part of it spells: `"1_000e999"` raises `FormatException`, not
//!   the `OverflowException` below.
//! cites: 9.2
//! expect: run
//! output: [1_000e999] -> FMT

module Conf;

public class Program
  public static method Main()
    Show("1_000e999");
  end Main;

  private static method Show(s: string)
    try
      _ := double.Parse(s);
      Console.WriteLine("[" + s + "] -> OK");
    catch
      FormatException => Console.WriteLine("[" + s + "] -> FMT");
    end;
  end Show;
end Program;
