//! case: s28_14_7201
//! doc: spec
//! section: 28.14
//! title: a constructor storing its plain function-typed parameter into a
//!   field is rejected
//! rule: It may not be stored in a field of any object
//! cites: 28.14, 22.7
//! expect: reject
//! diagnostic: stored
module Conf;

public class K
  public h: () -> int := lambda(): int => 0;

  public constructor(f: () -> int)
    h := f;
  end constructor;

  public method Peek(): int => h();
end K;

public class Program
  public static method Main()
    var s := "hi";
    var k := new K(lambda(): int => s.Length);
    Console.WriteLine(k.Peek());
  end Main;
end Program;
