//! case: s21_1_0801
//! doc: spec
//! section: 21.1
//! title: a local declared after a use does not shadow the built-in name at that earlier use
//! rule: is in scope from the end of its declaration to the end of the innermost enclosing statement block
//! cites: 21.1, 2.3, 2.7
//! expect: run
//! output: 4
//!   USER
//! issue: 8019
//! notes: the local Math is in scope only from the end of its declaration; the first call still reaches the always-available Math (2.3).
module Conf;

public class Sh
  public method Combine(a: string, b: string): string
    return "USER:" + a + "+" + b;
  end Combine;
  public method Sqrt(x: double): string
    return "USER";
  end Sqrt;
  public method Ping()
    Console.WriteLine("PING");
  end Ping;
  public method Assert(b: bool)
    Console.WriteLine("USERASSERT");
  end Assert;
  public property Pi: double => 3.25;
end Sh;

public class Program
  public static method Main()
    Console.WriteLine(Math.Sqrt(16.0));
    var Math := new Sh();
    Console.WriteLine(Math.Sqrt(16.0));
  end Main;
end Program;
