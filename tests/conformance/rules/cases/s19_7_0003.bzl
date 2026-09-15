//! case: s19_7_0003
//! doc: spec
//! section: 19.7
//! title: a case arm's do block closes with a bare end and no semicolon
//! rule: A `case` arm `do … end` block closes with a bare `end`, taking no
//!   `;` of its own — the arm is followed by the `,` separator (or nothing,
//!   if last), not a statement terminator.
//! cites: 19.2, 21
//! expect: reject
//! diagnostic: semicolon
module Conf;

public class Program
  public static method Main()
    var n := 1;
    case n of
      1 => do
        Console.WriteLine("one");
      end;,
      _ => Console.WriteLine("other")
    end case;
  end Main;
end Program;
