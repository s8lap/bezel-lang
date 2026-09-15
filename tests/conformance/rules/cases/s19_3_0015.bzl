//! case: s19_3_0015
//! doc: spec
//! section: 19.3
//! title: an unknown name in a named slot is a compile error
//! rule: An unknown name, a duplicate name in one slot list, a write-only
//!   property, an indexer, a static member, or a record method or
//!   non-public explicit field is a compile error at the pattern, each
//!   naming its rule.
//! cites: 19.3
//! expect: reject
//! diagnostic: unknown
module Conf;

public record Rect(Width: int, Height: int);

public class Program
  public static method Main()
    var r := new Rect(3, 4);
    case r of
      Rect(Width = var w, Depth = var d) => Console.WriteLine(w.ToString());,
      _                                  => Console.WriteLine("other");
    end case;
  end Main;
end Program;
