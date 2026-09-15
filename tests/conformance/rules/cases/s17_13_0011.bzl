//! case: s17_13_0011
//! doc: spec
//! section: 17.13
//! title: a for-loop variable _ iterates for effects with the element unbound
//! rule: "a **`for` loop variable** — `for var _ in xs do ... end;` iterates
//!   for effects with the element unbound, §29.2's semantics unchanged."
//! cites: 17.13, 21.4
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [10, 20, 30];
    var count := 0;
    for var _ in xs do
      count := count + 1;
    end;
    Console.WriteLine(count);
  end Main;
end Program;
