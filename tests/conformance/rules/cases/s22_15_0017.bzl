//! case: s22_15_0017
//! doc: spec
//! section: 22.15
//! title: a pure body may not construct a class instance outside the throw carve-out
//! rule: Prohibited: class construction — `new C(…)` ... except the throw
//!   operand below (constructors and destructors run arbitrary user code,
//!   and an owned instance drags in the discharge machinery).
//! cites: 22.15
//! expect: reject
//! diagnostic: class construction
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Ops
  public static pure method Make(): int
    var w := new Widget();
    return 0;
  end Make;
end Ops;
