//! case: s17_2_0013
//! doc: spec
//! section: 17.2
//! title: postfix ! binds tighter than unary minus, so -q! negates the asserted value
//! rule: ?., ?(...), and ! (postfix) are primary-level. They bind as tightly as .
//!   and [ ].
//! cites: 17.2, 17.1, 7.8, 7.11
//! expect: run
//! output: -5
//!   5
module Conf;

public class Program
  public static method Make(): int? => 5;

  public static method Main()
    var q: int? := Make();
    Console.WriteLine(-q!);
    Console.WriteLine(-(q ?? 1) + 10);
  end Main;
end Program;
