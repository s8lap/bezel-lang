//! case: s2_7_0804
//! doc: spec
//! section: 2.7
//! title: a local named Console receives ReadLine, not the built-in Console.ReadLine
//! rule: If it resolves to a *value* (a local, field, or property), the chain is member access.
//! cites: 2.7, 2.3
//! expect: run
//! stdout: UREAD
//! issue: 8126
//! notes: the built-in Console.ReadLine would read standard input; the result is the user method's constant.
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
    var Console := new Sh();
    var r := Console.ReadLine();
    Show(r);
  end Main;
end Program;
