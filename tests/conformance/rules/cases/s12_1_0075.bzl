//! case: s12_1_0075
//! doc: spec
//! section: 12.1
//! title: an override whose parameter type differs from the alias-expanded base parameter is rejected
//! rule: Visibility, return type, parameter types, and parameter modifiers must match the base member exactly
//! cites: 12.1, 27.8
//! expect: reject
//! diagnostic: parameter types
module Conf;

type Id<T> := T;

public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id<X>): string => "base";
end B;

public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method M(x: string): string => "derived";
end E;

public class M
  public static method Main(): int
    return 0;
  end Main;
end M;
