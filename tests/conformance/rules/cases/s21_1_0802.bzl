//! case: s21_1_0802
//! doc: spec
//! section: 21.1
//! title: a local in an inner block shadows only to the end of that block; the built-in is visible after it
//! rule: is in scope from the end of its declaration to the end of the innermost enclosing statement block
//! cites: 21.1, 2.3, 2.7
//! expect: run
//! output: USER
//!   3.25
//!   4
//!   3.141592653589793
//! issue: 8019
//! notes: Math.Pi after the block is the always-available constant (shortest round-trip rendering of the double).
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
    if true then
      var Math := new Sh();
      Console.WriteLine(Math.Sqrt(16.0));
      Console.WriteLine(Math.Pi);
    end if;
    Console.WriteLine(Math.Sqrt(16.0));
    Console.WriteLine(Math.Pi);
  end Main;
end Program;
