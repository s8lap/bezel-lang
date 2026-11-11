//! case: l9_1_4802
//! doc: stdlib
//! section: 9.1
//! title: long.Parse in radix 2 accepts the 64-bit minimum and raises the range failure one below it
//! rule: A `Parse` member — here, in [§9.2](#sec-double-and-float) and [§9.3](#sec-decimal), and through [§2.3](#sec-convert)'s `Convert.To*` — whose input is well-formed and spells a value the type cannot represent raises `ParseRangeException`
//! cites: 2.3
//! expect: run
//! output: ok:-9223372036854775808
//!   OVF
module Conf;

public class Program
  public static method P(s: string, r: int): string
    try
      var v := long.Parse(s, r);
      return "ok:" + v.ToString();
    catch
      FormatException => do return "FMT"; end,
      OverflowException => do return "OVF"; end
    end try;
  end P;

  public static method Main()
    Console.WriteLine(P("-1000000000000000000000000000000000000000000000000000000000000000", 2));
    Console.WriteLine(P("-1000000000000000000000000000000000000000000000000000000000000001", 2));
  end Main;
end Program;
