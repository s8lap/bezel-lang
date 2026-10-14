//! case: s12_1_0067
//! doc: spec
//! section: 12.1
//! title: an override returning a different class type parameter is rejected
//! rule: Return types must still match exactly (no covariance in this version).
//! cites: 12.1, 14.1
//! expect: reject
//! diagnostic: does not match the base member
module Conf;

public class B<T>
  public constructor() end constructor;
  public virtual method R(): T => throw new InvalidOperationException("x");
end B;

public class D<X, Y>: B<X>
  public constructor() end constructor;
  public override method R(): Y => throw new InvalidOperationException("y");
end D;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
