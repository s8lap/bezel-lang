//! case: s19_6_0002
//! doc: spec
//! section: 19.6
//! title: an unmatched statement-position case with no wildcard falls through
//! rule: A `case` with no `_` arm is permitted. In statement position, an
//!   unmatched value falls through with no action.
//! cites: 19.6
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static method Main()
    var n := 99;
    case n of
      1 => Console.WriteLine("one");,
      2 => Console.WriteLine("two");
    end case;
    Console.WriteLine("done");
  end Main;
end Program;
