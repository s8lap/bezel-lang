//! case: s13_7_0006
//! doc: spec
//! section: 13.7
//! title: an override matching the base member's visibility is admitted
//! rule: An override must declare the same visibility as the base member.
//! cites: 13.7, C.1, C.2
//! expect: accept
module Conf;

public class Base
  public constructor()
  end constructor;

  public virtual method Greet() end Greet;
end Base;

public class Derived: Base
  public constructor()
  end constructor;

  public override method Greet() end Greet;
end Derived;
