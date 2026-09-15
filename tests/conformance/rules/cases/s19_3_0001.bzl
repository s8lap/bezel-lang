//! case: s19_3_0001
//! doc: spec
//! section: 19.3
//! title: a binding may not appear inside an alternative pattern list
//! rule: In an alternative list, every member must be binding-free — a
//!   constant, a payload-free choice case, a range, or a relational
//!   pattern — so a binding (`var c`) cannot appear inside an alternative
//!   list.
//! cites: 19.3
//! expect: reject
//! diagnostic: binding
module Conf;

public enum Color
  Red, Green, Blue;
end Color;

public class Program
  public static method Main()
    var c := Color.Green;
    case c of
      var x | Color.Blue => Console.WriteLine("matched"),
      _                  => Console.WriteLine("other")
    end case;
  end Main;
end Program;
