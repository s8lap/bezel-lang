//! case: s2_7_0802
//! doc: spec
//! section: 2.7
//! title: a local named Console inside a generic method receives WriteLine at every instantiation
//! rule: If it resolves to a *value* (a local, field, or property), the chain is member access.
//! cites: 2.7, 2.3
//! expect: run
//! output: USER:g
//!   USER:g
//! issue: 8126
module Conf;

public class Sh
  public last: string := "";
  public method WriteLine(s: string)
    this.last := "USER:" + s;
  end WriteLine;
  public method ReadLine(): string
    return "UREAD";
  end ReadLine;
end Sh;

public class Program
  public static method G<T>(x: T): string
    var Console := new Sh();
    Console.WriteLine("g");
    return Console.last;
  end G;
  public static method Show(s: string)
    Console.WriteLine(s);
  end Show;
  public static method Main()
    Show(G<int>(1));
    Show(G<string>("s"));
  end Main;
end Program;
