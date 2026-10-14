//! case: s12_1_0053
//! doc: spec
//! section: 12.1
//! title: a property override through an indirect generic base dispatches through the base type
//! rule: A method, property, or indexer is non-virtual unless marked
//!   `virtual`. Visibility, return type, parameter types, and parameter
//!   modifiers must match the base member exactly (v0.338, §12.1). The
//!   property `V` that `E` inherits through `D<int>` is typed `int`, so
//!   `override property V: int` matches it.
//! cites: 12.1, 14.1
//! expect: run
//! issue: 6303
//! stdout: 9
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual property V: T => default(T);
end B;

public class D<T>: B<T>
  public constructor()
  end constructor;
end D;

public class E: D<int>
  public constructor()
  end constructor;

  public override property V: int => 9;
end E;

public class Program
  public static method Main(): int
    var d: B<int> := new E();
    Console.WriteLine(d.V);
    return 0;
  end Main;
end Program;
