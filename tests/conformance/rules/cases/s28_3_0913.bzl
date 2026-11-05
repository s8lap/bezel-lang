//! case: s28_3_0913
//! doc: spec
//! section: 28.3
//! title: a method that returns its plain function-typed parameter is rejected
//! rule: a store of a function value admits a capture-free value and rejects one whose source set
//!   is non-empty (a capture-bearing closure, a plain function-typed parameter, a local
//!   carrying either)
//! cites: 28.9, 27.5
//! expect: reject
//! diagnostic: function
//! issue: 8141
module Conf;

public class Holder
  public constructor()
  end constructor;

  public method GetP(f: (int) -> int): (int) -> int
    return f;
  end GetP;
end Holder;
