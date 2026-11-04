//! case: s7_4_0172
//! doc: spec
//! section: 7.4
//! title: the true branch of an `is not ... and` conjunction over a readonly function field is not
//!   narrowed
//! rule: in `A and B`, `B` (and the true branch) is checked under `A`'s true-facts
//! cites: 12.7
//! expect: reject
//! diagnostic: narrow
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
    if this._f is not (int) -> int and k = 1 then
      Console.WriteLine(this._f(1));
    end if;
  end Fire;
end K;

public class Program
  public static method Main()
    using var a := new K(true);
    a.Fire();
    using var b := new K(false);
    b.Fire();
  end Main;
end Program;
