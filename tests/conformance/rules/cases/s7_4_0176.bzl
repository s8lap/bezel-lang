//! case: s7_4_0176
//! doc: spec
//! section: 7.4
//! title: a readonly-field fact rooted at this enters a lambda formed under the guard
//! rule: a `readonly`-field path's rooted at a `const` local, at `this` or at a type name
//! cites: 27.5
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
    if this._f is (int) -> int then
      var g := lambda(): int => this._f(1);
      Console.WriteLine(g());
    else
      Console.WriteLine("none");
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
