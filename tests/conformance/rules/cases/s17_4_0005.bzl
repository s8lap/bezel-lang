//! case: s17_4_0005
//! doc: spec
//! section: 17.4
//! title: an inline-array as target is rejected
//! rule: An **inline-array target** (any form) is a compile error naming this
//!   rule: §15.1 admits no nullable inline-array form, so the operator's
//!   contract is unsatisfiable.
//! cites: 17.4, 15.1
//! expect: reject
//! diagnostic: inline array
module Conf;

public class Program
  public static method Check(o: Object): bool
    var r := o as [3]int;
    return r <> null;
  end Check;
end Program;
