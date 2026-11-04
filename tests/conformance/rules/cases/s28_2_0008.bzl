//! case: s28_2_0008
//! doc: spec
//! section: 28.2
//! title: a record member named Dispose is rejected
//! rule: Declaring a member named `Dispose` on a class, a record, an enum or a
//!   choice is a compile error at that declaration, whatever its parameter list
//!   and whatever its kind; on a record the name promises a disposal contract
//!   the kind does not have (§9.7), and so it does on an enum or a choice,
//!   neither of which has a `Dispose()` member (§8.7).
//! cites: 28.2, 9.7
//! expect: reject
//! diagnostic: dispose
//! issue: 4404
module Conf;

public record Setting
  public readonly property Mode: int;

  public constructor(mode: int)
    Mode := mode;
  end constructor;

  public method Dispose()
    Console.WriteLine("closing");
  end Dispose;
end Setting;
