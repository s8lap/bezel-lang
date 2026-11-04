//! case: s7_4_0164
//! doc: spec
//! section: 7.4
//! title: a readonly nullable function field tested with `this._f is` is invocable in the controlled block
//! rule: The tracked shape is exactly one field off an admitted root — `this._f`, `TypeName._s`, or
//!   `v._f`
//! cites: 12.7, 27.2
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
      Console.WriteLine(this._f(1));
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
