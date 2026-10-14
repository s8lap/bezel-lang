//! case: s12_1_0071
//! doc: spec
//! section: 12.1
//! title: an override with the same class type parameter as its method parameter is admitted
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
  public override method M(x: X): int => 2;
end D;
