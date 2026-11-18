//! case: s28_14_7203
//! doc: spec
//! section: 28.14
//! title: a constructor adding its plain function-typed parameter to a
//!   collection field is rejected
//! rule: stored into any collection, a local one included
//! cites: 28.14, 22.7
//! expect: reject
//! diagnostic: stored
module Conf;

public class K
  public owned l: Array<() -> int> := new Array<() -> int>();

  public constructor(f: () -> int)
    l.Add(f);
  end constructor;

  public method Peek(): int => l[0]();
end K;

public class Program
  public static method Main()
    var s := "hi";
    using var k := new K(lambda(): int => s.Length);
    Console.WriteLine(k.Peek());
  end Main;
end Program;
