//! case: s12_1_0070
//! doc: spec
//! section: 12.1
//! title: an indexer override with Array of a different class type parameter as its key is rejected
//! rule: Visibility, return type, parameter types, and parameter modifiers must match the base member exactly, with one allowance: an override may declare a parameter as `T?` where the overridden member declares `T`
//! cites: 12.1, 14.1
//! expect: reject
//! diagnostic: does not match the base member
module Conf;

public class B<T>
  public constructor() end constructor;
  public virtual property [i: Array<T>]: int => 1;
end B;

public class D<X, Y>: B<X>
  public constructor() end constructor;
  public override property [i: Array<Y>]: int => 2;
end D;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
