//! case: s13_6_0001
//! doc: spec
//! section: 13.6
//! title: an accessor visibility less restrictive than the property is rejected
//! rule: A property's `get` or `set` may carry its own visibility, which
//!   must not be less restrictive than the property's overall visibility.
//! cites: 13.6
//! expect: reject
//! diagnostic: less restrictive
module Conf;

public class Account
  private _name: string := "x";

  public constructor()
  end constructor;

  protected property Name: string
    get => _name;
    public set => _name := value;
  end Name;
end Account;
