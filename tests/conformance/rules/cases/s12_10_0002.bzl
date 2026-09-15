//! case: s12_10_0002
//! doc: spec
//! section: 12.10
//! title: a two-class inheritance cycle is rejected
//! rule: the general case is a chain (class A: B, class B: C, class C: A)
//!   and the mixed class/interface chains the relation admits.
//! cites: 12.10
//! expect: reject
//! diagnostic: cycle
module Conf;

public class A: B
  public constructor()
  end constructor;
end A;

public class B: A
  public constructor()
  end constructor;
end B;
