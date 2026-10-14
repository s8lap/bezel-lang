//! case: s12_1_0061
//! doc: spec
//! section: 12.1
//! title: a generic-method override whose substituted parameter type differs from the base's is rejected
//! rule: Visibility, return type, parameter types, and parameter modifiers
//!   must match the base member exactly (v0.338, §12.1). The base member
//!   that `C` overrides through `B<int>` takes `t: int`, so an override
//!   declaring `t: string` does not match it.
//! cites: 12.1, 14.1
//! expect: reject
//! diagnostic: does not match
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual method Q<U>(e: U, t: T): int => 1;
end B;

public class C: B<int>
  public constructor()
  end constructor;

  public override method Q<V>(e: V, t: string): int => 2;
end C;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
