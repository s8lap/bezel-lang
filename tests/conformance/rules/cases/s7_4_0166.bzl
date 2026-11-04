//! case: s7_4_0166
//! doc: spec
//! section: 7.4
//! title: the else branch of `is not` over a readonly function-typed field is narrowed
//! rule: an `elif` condition, its branch and the `else` branch, written or implicit, lie on the paths on
//!   which every earlier condition of the chain was false and carry those conditions' false-facts
//! cites: 12.7
//! expect: run
//! output: 2
//!   none
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
      Console.WriteLine("none");
    else
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
