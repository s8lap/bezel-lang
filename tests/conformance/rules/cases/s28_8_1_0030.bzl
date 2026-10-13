//! case: s28_8_1_0030
//! doc: spec
//! section: 28.8.1
//! title: the bounds check of a slot exchange runs after the replacement is evaluated
//! rule: The checks that read the location's current state — an owning
//!   indexer's bounds and key checks, and `!`'s null test ... — run when `e`
//!   has been evaluated, immediately before the exchange, so nothing `e`
//!   does (a call that resizes the collection or empties the field, a
//!   suspension at an `await`) can invalidate them.
//! cites: 28.8.1, 28.11
//! expect: run
//! stdout: added fresh 2
//! issue: 5923
module Conf;

public class Res
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Res;

public class Program
  public static method Grow(xs: OwningArray<Res>): owned Res
    xs.Add(new Res("added"));
    return new Res("fresh");
  end Grow;

  public static method Main()
    using var xs := new OwningArray<Res>();
    xs.Add(new Res("a"));
    using var t := move xs[1] with Grow(xs);
    Console.WriteLine($"{t.Name} {xs[1].Name} {xs.Count}");
  end Main;
end Program;
