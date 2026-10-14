//! case: s12_1_0063
//! doc: spec
//! section: 12.1
//! title: an override whose return type differs from the substituted base return type is rejected
//! rule: Return types must still match exactly (no covariance in this version)
//!   (v0.338, §12.1). `C` derives `B<int>`, whose virtual `Make()` returns
//!   `int`; an override returning `string` does not match it.
//! cites: 12.1, 14.1
//! expect: reject
//! diagnostic: does not match
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual method Make(): T => default(T);
end B;

public class C: B<int>
  public constructor()
  end constructor;

  public override method Make(): string => "x";
end C;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
