//! case: s19_3_0005
//! doc: spec
//! section: 19.3
//! title: a relational pattern admits only the four ordering operators
//! rule: A relational pattern is `> 70`, `< 50`, `<= 100`, `>= 0` — these
//!   four operators only.
//! cites: 19.3
//! expect: reject
//! diagnostic: pattern
module Conf;

public class Program
  public static method Main()
    var n := 5;
    case n of
      <> 0 => Console.WriteLine("nonzero"),
      _    => Console.WriteLine("zero")
    end case;
  end Main;
end Program;
