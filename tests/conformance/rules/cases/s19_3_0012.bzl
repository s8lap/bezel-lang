//! case: s19_3_0012
//! doc: spec
//! section: 19.3
//! title: a discard used as a top-level catch-all arm must be last
//! rule: When used as a top-level pattern (a catch-all arm), `_` must be
//!   the last arm; placing it earlier is a compile error.
//! cites: 19.3, 19.6
//! expect: reject
//! diagnostic: last
module Conf;

public class Program
  public static method Main()
    var n := 5;
    case n of
      _ => Console.WriteLine("any");,
      1 => Console.WriteLine("one");
    end case;
  end Main;
end Program;
