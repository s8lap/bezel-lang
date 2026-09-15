//! case: s12_10_0005
//! doc: spec
//! section: 12.10
//! title: a three-class inheritance chain that returns to its start is rejected
//! rule: The general case is a chain (`class A: B`, `class B: C`,
//!   `class C: A`); the compiler detects the cycle and reports an error naming
//!   the participating types.
//! cites: 12.10, C.1
//! expect: reject
//! diagnostic: cycle
module Conf;

public class A: B
  public constructor()
  end constructor;
end A;

public class B: C
  public constructor()
  end constructor;
end B;

public class C: A
  public constructor()
  end constructor;
end C;
