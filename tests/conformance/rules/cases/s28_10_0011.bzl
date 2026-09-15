//! case: s28_10_0011
//! doc: spec
//! section: 28.10
//! title: the source of an unmarked using var adoption is left moved-from
//! rule: Under the unmarked move-only adoption, `x` is left **moved-from** —
//!   the plain read moved the value — and reading from a moved-from location
//!   is a compile error (§15.3). The whole-value move leaves the local holding
//!   no value rather than a hole, so it owes no restoration.
//! cites: 28.10, 15.3
//! expect: reject
//! diagnostic: moved-from
//! issue: 4376
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Run()
    var a: [1 owned]Widget := [new Widget()];
    using var b := a;
    using var c := a;
  end Run;
end Program;
