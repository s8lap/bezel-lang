//! case: s16_6_0004
//! doc: spec
//! section: 16.6
//! title: a constant reversed range pattern warns that it never matches
//! rule: "Because a constant reversed range pattern can never match, the compiler
//!   emits a warning at the pattern ('this pattern never matches — bounds are
//!   reversed')."
//! cites: 16.6
//! expect: warn
//! diagnostic: never matches
module Conf;

public class Program
  public static method Main()
    var x: int := 50;
    case x of
      100..0 => Console.WriteLine("matched"),
      _      => Console.WriteLine("fallback")
    end case;
  end Main;
end Program;
