//! case: s13_1_0007
//! doc: spec
//! section: 13.1
//! title: internal does not extend to a derived class in another module
//! rule: `internal` and `protected` are incomparable — a derived class in a
//!   different module sees `protected` but not `internal`. Inheriting from a
//!   class in another module confers no access to that class's internal
//!   members.
//! cites: 13.1
//! expect: reject
//! diagnostic: internal
module App;
import Lib;

public class Derived: Base
  public constructor(secret: int)
    base(secret);
  end constructor;

  public method Reveal(): int => _secret;
end Derived;
