//! case: s14_2_0086
//! doc: spec
//! section: 14.2
//! title: a base clause naming a generic class at an argument that fails its bound is rejected
//! rule: A generic type is instantiated by being named, including in a base
//!   clause; every such reference is an instantiation site, and this
//!   section's constraint checks fire at every such site (v0.338, §14.1,
//!   §14.2). `Derived` names `Box<Other>` in its base clause, and `Other`
//!   does not conform to `IFoo`, the bound of `Box`'s `T`.
//! cites: 14.1, 14.2
//! expect: reject
//! diagnostic: IFoo
module Conf;

public interface IFoo
  method Ping();
end IFoo;

public class Box<T> where T: IFoo
  public constructor()
  end constructor;
end Box;

public class Other
  public constructor()
  end constructor;
end Other;

public class Derived: Box<Other>
  public constructor()
  end constructor;
end Derived;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
