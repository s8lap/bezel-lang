//! case: s21_1_0002
//! doc: spec
//! section: 21.1
//! title: a local cannot be named the discard
//! rule: A local cannot be named `_` — the bare underscore is the reserved
//!   discard token. `var _ := e;` is a compile error whose diagnostic
//!   names both sanctioned spellings.
//! cites: 21.1, 17.13
//! expect: reject
//! diagnostic: discard
module Conf;

public class Program
  public static method Main()
    var _ := 5;
    Console.WriteLine("done");
  end Main;
end Program;
