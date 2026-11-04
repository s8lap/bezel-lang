//! case: s7_4_0159
//! doc: spec
//! section: 7.4
//! title: check-then-use of a mutably captured nullable function local with no call between is accepted
//! rule: Straight-line check-then-use of a captured local (`if x <> null then x.M(); end;` — no call
//!   *between* the test and the use) remains accepted.
//! cites: 27.5
//! expect: run
//! output: 2
module Conf;

public class Program
  public static method Pick(): ((int) -> int)?
    return lambda(x: int) => x + 1;
  end Pick;

  public static method Main()
    var h: ((int) -> int)? := Pick();
    var clear := lambda() => do h := null; end;
    var r := 0;
    if h <> null then
      r := h(1);
    end if;
    clear();
    Console.WriteLine(r);
  end Main;
end Program;
