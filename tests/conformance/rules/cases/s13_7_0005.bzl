//! case: s13_7_0005
//! doc: spec
//! section: 13.7
//! title: an override declaring a different visibility than the base member is rejected
//! rule: An override must declare the same visibility as the base member.
//! cites: 13.7, C.1, C.2
//! expect: reject
//! diagnostic: same visibility
module Conf;

public class Base
  public constructor()
  end constructor;

  public virtual method Greet() end Greet;
end Base;

public class Derived: Base
  public constructor()
  end constructor;

  protected override method Greet() end Greet;
end Derived;
