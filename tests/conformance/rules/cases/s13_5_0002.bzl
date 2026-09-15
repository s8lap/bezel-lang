//! case: s13_5_0002
//! doc: spec
//! section: 13.5
//! title: protected alone is a valid single visibility
//! rule: Bezel does not provide combined forms (no `protected internal`, no
//!   `private protected`); a single level is unaffected.
//! cites: 13.5
//! expect: accept
module Conf;

public class Base
  protected _value: int;

  public constructor()
    _value := 0;
  end constructor;
end Base;
