//! case: s7_4_0165
//! doc: spec
//! section: 7.4
//! title: the true branch of `is not` over a readonly function-typed field carries no non-null fact
//! rule: or a successful `is`/typed-pattern test
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
    if this._f is not (int) -> int then
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
