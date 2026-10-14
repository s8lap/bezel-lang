//! case: s12_1_0072
//! doc: spec
//! section: 12.1
//! title: an override whose parameter has a different name but the base's class type parameter is admitted
//! rule: Visibility, return type, parameter types, and parameter modifiers must match the base member exactly, with one allowance: an override may declare a parameter as `T?` where the overridden member declares `T`
//! cites: 12.1, 14.1
//! expect: accept
module Conf;

public class B<T>
  public constructor() end constructor;
  public virtual method M(x: T): int => 1;
end B;

public class D<X>: B<X>
  public constructor() end constructor;
  public override method M(y: X): int => 2;
end D;
