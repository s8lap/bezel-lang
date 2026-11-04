//! case: s2_3_0802
//! doc: spec
//! section: 2.3
//! title: lambda parameters named Path and Math shadow the always-available names inside the lambda body
//! rule: every other always-available name may be shadowed by local declarations under the normal name-resolution rules
//! cites: 2.3, 2.7
//! expect: run
//! output: USER:a+b
//!   3.25
//! issue: 8019
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
    var f := lambda(Path: Sh) => Path.Combine("a", "b");
    Console.WriteLine(f(new Sh()));
    var g := lambda(Math: Sh) => Math.Pi;
    Console.WriteLine(g(new Sh()));
  end Main;
end Program;
