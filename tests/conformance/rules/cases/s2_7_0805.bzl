//! case: s2_7_0805
//! doc: spec
//! section: 2.7
//! title: a chain through a local named Console (Console.Out.WriteLine) is member access on the local
//! rule: If it resolves to a *value* (a local, field, or property), the chain is member access.
//! cites: 2.7, 2.3
//! expect: run
//! stdout: WOUT:o
//! issue: 8126
module Conf;

public class W
  public last: string := "";
  public method WriteLine(s: string)
    this.last := "WOUT:" + s;
  end WriteLine;
end W;

public class Sh
  public owned Out: W := new W();
end Sh;

public class Program
  public static method Show(x: string)
    Console.WriteLine(x);
  end Show;
  public static method Main()
    var Console := new Sh();
    Console.Out.WriteLine("o");
    Show(Console.Out.last);
  end Main;
end Program;
