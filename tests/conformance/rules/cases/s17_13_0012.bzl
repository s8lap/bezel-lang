//! case: s17_13_0012
//! doc: spec
//! section: 17.13
//! title: var _ := e; is rejected — _ is a discard, not a declarable name
//! rule: "`var _ := e;` and `const _ := e;` (`_` is a discard, not a
//!   declarable name — the diagnostic names both remedies, `_ := e;` for
//!   end-of-statement release or `using var _ := e;` for the anonymous
//!   scope-end hold)."
//! cites: 17.13, 21.1
//! expect: reject
//! diagnostic: discard
module Conf;

public class Program
  public static method Main()
    var _ := 5;
  end Main;
end Program;
