//! case: l2_18_0017
//! doc: stdlib
//! section: 2.18
//! title: the intrinsic vocabulary has no C specifier
//! rule: stdlib §2.18: "`C` (currency) deliberately does not exist — currency
//!     formatting is culture by definition" and "An unrecognized specifier
//!     raises `FormatException`". `C` is not in the intrinsic vocabulary, so it
//!     is an unrecognized specifier.
//! cites: 2.18
//! expect: run
//! stdout: caught
//! issue: 7303
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine((1.5).ToString("C"));
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
