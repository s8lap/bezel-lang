//! case: s28_8_1_0017
//! doc: spec
//! section: 28.8.1
//! title: move with on a non-nullable field yields the old value and the field holds the replacement at once
//! rule: `move loc with e` — `loc` an owned field `x.f` or an owning slot
//!   `coll[i]`, `e` an expression — evaluates `loc`'s receiver and index,
//!   then `e`, and then, in one step with no intermediate state, stores
//!   `e`'s value into the location and yields the location's previous
//!   value, owned.
//! cites: 28.8.1, 28.3
//! expect: run
//! output: second
//!   first
//! issue: 5923
module Conf;

public class Res
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Res;

public class Holder
  private owned _r: Res := new Res("first");

  public method Swap(): owned Res
    var old := move _r with new Res("second");
    Peek();
    return old;
  end Swap;

  private method Peek() => Console.WriteLine(_r.Name);
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    using var t := h.Swap();
    Console.WriteLine(t.Name);
  end Main;
end Program;
