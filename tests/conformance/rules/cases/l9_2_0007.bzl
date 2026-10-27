//! case: l9_2_0007
//! doc: stdlib
//! section: 9.2
//! title: double.Parse raises OverflowException on an input spelling an unrepresentable number
//! rule: So `double.Parse("1.7976931348623158e308")` is `double.MaxValue` and
//!   `float.Parse("3.4028235e38")` is `float.MaxValue`, while
//!   `double.Parse("1e999")`, `double.Parse("-1e999")` and
//!   `float.Parse("3.5e38")` raise.
//! cites: 9.2
//! expect: run
//! output: [1e999] -> OVF
//!   [-1e999] -> OVF

module Conf;

public class Program
  public static method Main()
    Show("1e999");
    Show("-1e999");
  end Main;

  private static method Show(s: string)
    try
      _ := double.Parse(s);
      Console.WriteLine("[" + s + "] -> OK");
    catch
      OverflowException => Console.WriteLine("[" + s + "] -> OVF");
    end;
  end Show;
end Program;
