//! case: s7_4_0173
//! doc: spec
//! section: 7.4
//! title: the true branch of an `is ... or` disjunction over a readonly function-typed field is not
//!   narrowed
//! rule: a fact holds at a point only when it holds on every path that reaches it
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
    if this._f is (int) -> int or k = 1 then
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
