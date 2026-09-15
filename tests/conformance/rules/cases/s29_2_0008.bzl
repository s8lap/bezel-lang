//! case: s29_2_0008
//! doc: spec
//! section: 29.2
//! title: default iteration over an OwningArray takes the borrowing path
//! rule: If `source`'s static type implements both `IEnumerable<T>` and
//!   `IOwningEnumerable<T>` (as `OwningArray<T>` does), the desugaring uses
//!   the `IEnumerable<T>` path — default iteration is borrowing.
//! cites: 29.2
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Report(pool: OwningArray<Widget>)
    for var w in pool do
      _ := w;
    end;
    Console.WriteLine(pool.Count);
  end Report;

  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    Report(pool);
  end Main;
end Program;
