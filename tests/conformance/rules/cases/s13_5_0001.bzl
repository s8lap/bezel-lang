//! case: s13_5_0001
//! doc: spec
//! section: 13.5
//! title: protected internal is not a valid combined visibility
//! rule: Bezel does not provide combined forms (no `protected internal`, no
//!   `private protected`).
//! cites: 13.5
//! expect: reject
//! diagnostic: combined
module Conf;

public class Base
  protected internal _value: int;

  public constructor()
    _value := 0;
  end constructor;
end Base;
