//! case: s29_2_0010
//! doc: spec
//! section: 29.2
//! title: an undischarged owned Drain() iteration variable auto-drops with a per-iteration must-use warning
//! rule: Omit the adoption and the element auto-drops each iteration with a
//!   **per-iteration must-use warning**.
//! cites: 29.2, 28.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    for var conn in pool.Drain() do
      Console.WriteLine("iter");
    end;
  end Main;
end Program;
