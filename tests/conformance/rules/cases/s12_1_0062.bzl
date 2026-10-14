//! case: s12_1_0062
//! doc: spec
//! section: 12.1
//! title: a parameter widened to T? mid-chain cannot be narrowed back to T further down
//! rule: Widening is monotone down the chain: an override matches against the
//!   nearest overridden declaration, so once a parameter is `T?` a further
//!   override must keep it `T?` (v0.338, §12.1); narrowing back would let a
//!   `null` admitted through an intermediate static type reach a
//!   non-nullable parameter. `D` overrides `B.Q(t: int?)`, so
//!   `override method Q(t: int)` is rejected.
//! cites: 12.1
//! expect: reject
//! diagnostic: does not match
module Conf;

public class A<T>
  public constructor()
  end constructor;

  public virtual method Q(t: T): int => 1;
end A;

public class B: A<int>
  public constructor()
  end constructor;

  public override method Q(t: int?): int => 2;
end B;

public class D: B
  public constructor()
  end constructor;

  public override method Q(t: int): int => 3;
end D;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
