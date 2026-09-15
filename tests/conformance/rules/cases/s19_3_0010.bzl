//! case: s19_3_0010
//! doc: spec
//! section: 19.3
//! title: individual positions of a positional destructure may be discarded
//! rule: The type name followed by parenthesized bindings matches the type
//!   and binds positional fields by position; individual positions may be
//!   discarded with `_` — `Point(_, y)`.
//! cites: 19.3
//! expect: run
//! stdout: 1,3
module Conf;

public record Triple(A: int, B: int, C: int);

public class Program
  public static method Main()
    var t := new Triple(1, 2, 3);
    case t of
      Triple(x, _, z) => Console.WriteLine($"{x},{z}")
    end case;
  end Main;
end Program;
