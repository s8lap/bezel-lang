//! case: s7_4_0179
//! doc: spec
//! section: 7.4
//! title: a readonly function-typed field off a non-ref parameter root is narrowed by an `is` test
//! rule: The tracked shape is exactly one field off an admitted root — `this._f`, `TypeName._s`, or
//!   `v._f`
//! cites: 12.7
//! expect: run
//! output: 2
//! issue: 8099
module Conf;

public class H
  public readonly F: ((int) -> int)?;

  public constructor()
    F := lambda(x: int) => x + 1;
  end constructor;
end H;

public class Program
  public static method Run(h: H): int
    if h.F is (int) -> int then
      return h.F(1);
    end if;
    return 0;
  end Run;

  public static method Main()
    using var h := new H();
    Console.WriteLine(Run(h));
  end Main;
end Program;
