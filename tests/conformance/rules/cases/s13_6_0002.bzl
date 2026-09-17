//! case: s13_6_0002
//! doc: spec
//! section: 13.6
//! title: a more restrictive accessor visibility is admitted
//! rule: A property's `get` or `set` may carry its own visibility, which
//!   must not be less restrictive than the property's overall visibility;
//!   a more restrictive accessor is the useful form.
//! cites: 13.6
//! expect: accept
module Conf;

public class Account
  private _name: string := "x";

  public constructor()
  end constructor;

  protected property Name: string
    get => _name;
    private set => _name := value;
  end Name;
end Account;
