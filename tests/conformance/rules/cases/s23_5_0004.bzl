//! case: s23_5_0004
//! doc: spec
//! section: 23.5
//! title: a named argument at an index position is rejected
//! rule: Index arguments are positional-only; the named-argument form
//!   (`Identifier :=`) is not admitted at an index position — a compile error
//!   naming this rule.
//! cites: 23.5, 22.5
//! expect: reject
//! diagnostic: named
module Conf;

public class Names
  private owned _items: Array<string> := new Array<string>();

  public constructor()
  end constructor;

  public property [i: int]: string => _items[i];
end Names;

public class Program
  public static method Main()
    using var n := new Names();
    Console.WriteLine(n[i := 0]);
  end Main;
end Program;
