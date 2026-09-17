//! case: s12_1_0003
//! doc: spec
//! section: 12.1
//! title: an override may widen a parameter from T to T?
//! rule: An override may declare a parameter as T? where the overridden
//!   member declares T -- the same contravariant nullability widening §11.2
//!   grants interface implementations.
//! cites: 12.1, 11.2
//! expect: accept
module Conf;

public class Greeter
  public constructor()
  end constructor;

  public virtual method Greet(name: string): string => $"Hello, {name}";
end Greeter;

public class LenientGreeter: Greeter
  public constructor()
  end constructor;

  public override method Greet(name: string?): string => $"Hello, {name ?? "stranger"}";
end LenientGreeter;
