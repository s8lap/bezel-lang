//! case: s19_3_0002
//! doc: spec
//! section: 19.3
//! title: a binding-free alternative list matches any listed member
//! rule: `2 | 3 | 4 | 5` matches any of these — every member of an
//!   alternative list must be binding-free: a constant, a payload-free
//!   choice case, a range, or a relational pattern.
//! cites: 19.3
//! expect: run
//! stdout: warm-ish
module Conf;

public enum Color
  Red, Green, Blue;
end Color;

public class Program
  public static method Main()
    var c := Color.Green;
    case c of
      Color.Red | Color.Green => Console.WriteLine("warm-ish"),
      _                       => Console.WriteLine("other")
    end case;
  end Main;
end Program;
