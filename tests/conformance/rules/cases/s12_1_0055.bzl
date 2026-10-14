//! case: s12_1_0055
//! doc: spec
//! section: 12.1
//! title: an indexer override through an indirect generic base dispatches through the base type
//! rule: A method, property, or indexer is non-virtual unless marked
//!   `virtual`. Visibility, return type, parameter types, and parameter
//!   modifiers must match the base member exactly (v0.338, §12.1). The
//!   indexer `[i: int]` that `E` inherits through `D<int>` has the key
//!   `int` and the element type `int`, so `override property [i: int]: int`
//!   matches it.
//! cites: 12.1, 14.1
//! expect: run
//! issue: 6303
//! stdout: 42
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual property [i: int]: T => default(T);
end B;

public class D<T>: B<T>
  public constructor()
  end constructor;
end D;

public class E: D<int>
  public constructor()
  end constructor;

  public override property [i: int]: int => i + 40;
end E;

public class Program
  public static method Main(): int
    var d: B<int> := new E();
    Console.WriteLine(d[2]);
    return 0;
  end Main;
end Program;
