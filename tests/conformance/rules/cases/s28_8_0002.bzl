//! case: s28_8_0002
//! doc: spec
//! section: 28.8
//! title: implicit boxing produces no disposal obligation and draws no discharge diagnostic
//! rule: Implicit boxing is not subject to the discharge rule of this
//!   section — when a value type is boxed to be used where `Object` is
//!   expected, the boxed wrapper is a runtime-managed value, not an owned
//!   `new`-result; the boxing itself is not a `new` expression and produces
//!   no disposal obligation.
//! cites: 28.8, 28.9, 8.6
//! expect: quiet
module Conf;

public class Reporter
  public constructor()
  end constructor;

  public method Report()
    var boxed: Object := 42;
    Console.WriteLine(boxed);
  end Report;
end Reporter;
