//! case: s19_3_0013
//! doc: spec
//! section: 19.3
//! title: a discard as the last arm is the idiomatic catch-all
//! rule: "`_` is the idiomatic spelling of an arm that matches everything;
//!   when used as a top-level pattern it must be the last arm."
//! cites: 19.3, 19.6
//! expect: run
//! stdout: any
module Conf;

public class Program
  public static method Main()
    var n := 5;
    case n of
      1 => Console.WriteLine("one");,
      _ => Console.WriteLine("any");
    end case;
  end Main;
end Program;
