//! case: s19_3_0006
//! doc: spec
//! section: 19.3
//! title: a destructuring slot list may not mix positional and named slots
//! rule: A slot list is either positional or named, never mixed in one
//!   pattern; mixing is a compile error naming the rule.
//! cites: 19.3
//! expect: reject
//! diagnostic: positional
module Conf;

public record Rect(Width: int, Height: int);

public class Program
  public static method Main()
    var r := new Rect(3, 4);
    case r of
      Rect(3, Height = var h) => Console.WriteLine(h.ToString());,
      _                       => Console.WriteLine("other");
    end case;
  end Main;
end Program;
