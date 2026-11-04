//! case: s7_4_0168
//! doc: spec
//! section: 7.4
//! title: an early return on `is not ... or` over a readonly function-typed field narrows the surviving
//!   path
//! rule: in `A or B`, `B` (and the false branch) sees `A`'s false-facts
//! cites: 12.7
//! expect: run
//! output: 2
//!   a
//!   b
//! issue: 8099
module Conf;

public class K
  private readonly _f: ((int) -> int)?;

  public constructor(present: bool)
    if present then
      _f := lambda(x: int) => x + 1;
    else
      _f := null;
    end if;
  end constructor;

  public method Fire()
    var k := 1;
    if this._f is not (int) -> int or k < 0 then
      return;
    end if;
    Console.WriteLine(this._f(1));
  end Fire;
end K;

public class Program
  public static method Main()
    using var a := new K(true);
    a.Fire();
    Console.WriteLine("a");
    using var b := new K(false);
    b.Fire();
    Console.WriteLine("b");
  end Main;
end Program;
