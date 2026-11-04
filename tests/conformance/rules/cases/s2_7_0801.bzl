//! case: s2_7_0801
//! doc: spec
//! section: 2.7
//! title: a local named Console in an inner block receives WriteLine; the built-in Console is back after it
//! rule: If it resolves to a *value* (a local, field, or property), the chain is member access.
//! cites: 2.7, 2.3
//! expect: run
//! output: USER:in
//!   after
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
  public static method Show(s: string)
    Console.WriteLine(s);
  end Show;
  public static method Main()
    if true then
      var Console := new Sh();
      Console.WriteLine("in");
      Show(Console.last);
    end if;
    Console.WriteLine("after");
  end Main;
end Program;
