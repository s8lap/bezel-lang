//! case: s11_1_0002
//! doc: spec
//! section: 11.1
//! title: two base classes on one class is rejected
//! rule: A class may declare at most one base class and any number of
//!   interfaces.
//! cites: 11.1
//! expect: reject
//! diagnostic: base class
module Conf;

public class A
  public constructor()
  end constructor;
end A;

public class B
  public constructor()
  end constructor;
end B;

public class C: A, B
  public constructor()
  end constructor;
end C;
