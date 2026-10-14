//! case: s12_1_0054
//! doc: spec
//! section: 12.1
//! title: a property override with the wrong type through an indirect generic base is rejected
//! rule: Visibility, return type, parameter types, and parameter modifiers
//!   must match the base member exactly (v0.338, §12.1), and the property
//!   `V` that `E` inherits through `D<int>` is typed `int`; an override
//!   declaring `property V: string` does not match it.
//! cites: 12.1, 14.1
//! expect: reject
//! diagnostic: does not match
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

  public override property V: string => "x";
end E;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
