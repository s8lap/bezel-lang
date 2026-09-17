//! case: s9_5_0005
//! doc: spec
//! section: 9.5
//! title: a with target that is a private auto-property is legal from inside the declaring record
//! rule: A private auto-property is a valid with target inside the declaring
//!   record.
//! cites: 13
//! expect: accept
module Conf;

public record Account
  private readonly property Secret: int;
  public readonly property Id: int;

  public constructor(id: int, secret: int)
    Id := id;
    Secret := secret;
  end constructor;

  public method Reset(): Account => this with(Secret := 0);
end Account;
