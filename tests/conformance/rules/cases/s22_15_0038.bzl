//! case: s22_15_0038
//! doc: spec
//! section: 22.15
//! title: a pure body may not construct a stdlib collection class
//! rule: except the throw operand below (constructors and destructors run
//!   arbitrary user code, and an owned instance drags in the discharge
//!   machinery)
//! cites: 22.15
//! expect: reject
//! diagnostic: class construction
//! issue: 7453
module Conf;

public class Ops
  public static pure method Make(): int
    var d := new Dictionary<string, int>();
    return d.Count;
  end Make;
end Ops;
