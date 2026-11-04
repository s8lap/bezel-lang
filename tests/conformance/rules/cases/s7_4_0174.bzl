//! case: s7_4_0174
//! doc: spec
//! section: 7.4
//! title: the else arm of a conditional atom over a readonly function field is on the failed-test path
//! rule: an `elif` condition, its branch and the `else` branch, written or implicit, lie on the paths on
//!   which every earlier condition of the chain was false and carry those conditions' false-facts
//! cites: 20, 12.7
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
    var r := (if this._f is (int) -> int then 0 else this._f(1));
    Console.WriteLine(r);
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
