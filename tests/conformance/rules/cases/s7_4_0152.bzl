//! case: s7_4_0152
//! doc: spec
//! section: 7.4
//! title: a readonly nullable field narrowed by a null test is admitted at Math.Min
//! rule: A nullable **`readonly`** field — instance or `static` — participates in flow narrowing
//! cites: 2.2
//! expect: run
//! output: 3
//!   0
//! issue: 7660
//! notes: Accepting twin of s7_4_0151.
module Conf;

public class Box
  private readonly _v: int?;

  public constructor(v: int?)
    _v := v;
  end constructor;

  public method Get(): int
    if _v <> null then
      return Math.Min(_v, 5);
    end if;
    return 0;
  end Get;
end Box;

public class Program
  public static method Main()
    using var b := new Box(3);
    Console.WriteLine(b.Get());
    using var c := new Box(null);
    Console.WriteLine(c.Get());
  end Main;
end Program;
