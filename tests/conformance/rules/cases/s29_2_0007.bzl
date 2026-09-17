//! case: s29_2_0007
//! doc: spec
//! section: 29.2
//! title: iterating a using-var collection pins it for the loop without taking ownership
//! rule: When `source` is itself an owned local — including a `using var` of
//!   class type — the desugaring is adjusted: `using var __source := source;`
//!   is replaced by a borrow that pins `source` for the loop's duration
//!   without assuming the disposal obligation.
//! cites: 29.2
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    for var w in pool do
      _ := w;
    end;
    Console.WriteLine(pool.Count);
  end Main;
end Program;
