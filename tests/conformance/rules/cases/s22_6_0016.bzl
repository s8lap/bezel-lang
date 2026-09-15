//! case: s22_6_0016
//! doc: spec
//! section: 22.6
//! title: scoped never distinguishes overloads
//! rule: "`scoped` does not participate in overload resolution: two
//!   overloads differing only in `scoped` on a parameter ... are a compile
//!   error."
//! cites: 22.6
//! expect: reject
//! diagnostic: scoped
module Conf;

public class Sub
  public method Register(cb: () -> ())
  end Register;

  public method Register(scoped cb: () -> ())
  end Register;
end Sub;
