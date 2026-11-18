//! case: s28_14_7202
//! doc: spec
//! section: 28.14
//! title: a constructor storing its plain function-typed parameter inside a
//!   stored closure is rejected
//! rule: captured by a closure that itself escapes the callee (judged by
//!   these same rules, applied transitively), or forwarded into a `scoped`
//!   parameter position
//! cites: 28.14, 22.7
//! expect: reject
//! diagnostic: scoped
module Conf;

public class K
  public h: () -> int := lambda(): int => 0;

  public constructor(f: () -> int)
    h := lambda(): int => f() + 1;
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
