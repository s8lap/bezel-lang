//! case: s12_1_0052
//! doc: spec
//! section: 12.1
//! title: a non-generic override with a wrong parameter type through an indirect generic base is rejected
//! rule: Visibility, return type, parameter types, and parameter modifiers
//!   must match the base member exactly (v0.338, §12.1). `E2` derives
//!   `D<int>`, which derives `B<T>`; the base member it overrides is
//!   `Show(a: int)` under the base-clause arguments (§14.1), so
//!   `override method Show(a: string)` does not match it.
//! cites: 12.1, 14.1
//! expect: reject
//! diagnostic: does not match
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual method Show(a: T): int => 1;
end B;

public class D<T>: B<T>
  public constructor()
  end constructor;
end D;

public class E2: D<int>
  public constructor()
  end constructor;

  public override method Show(a: string): int => 7;
end E2;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
