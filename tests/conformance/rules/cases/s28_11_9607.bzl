//! case: s28_11_9607
//! doc: spec
//! section: 28.11
//! title: a scope-bound builtin local stored into a collection a plain parameter holds is rejected
//! rule: is held by a parameter that is not `owned` or reached through one (caller
//!   storage, which outlives every scope of the body)
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlive
//! issue: 9182
module Conf;

public class C
  public method Fill(d: Dictionary<string, Dictionary<string, int>>)
    var inner := new Dictionary<string, int>();
    inner["x"] := 1;
    d["k"] := inner;
  end Fill;
end C;
