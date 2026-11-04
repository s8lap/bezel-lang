//! case: s7_4_0171
//! doc: spec
//! section: 7.4
//! title: a fact from an `is` test on a readonly function-typed field does not hold after the if statement
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
    if this._f is (int) -> int then
      Console.WriteLine("in");
    end if;
    Console.WriteLine(this._f(1));
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
