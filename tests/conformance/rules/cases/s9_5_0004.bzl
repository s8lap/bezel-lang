//! case: s9_5_0004
//! doc: spec
//! section: 9.5
//! title: a with target must be accessible at the use site under its declared accessibility
//! rule: A with target must be accessible at the use site under the member's own
//!   declared accessibility; a private auto-property is a valid target only
//!   inside the declaring record.
//! cites: 13
//! expect: reject
//! diagnostic: accessible
module Conf;

public record Account
  private readonly property Secret: int;
  public readonly property Id: int;

  public constructor(id: int, secret: int)
    Id := id;
    Secret := secret;
  end constructor;
end Account;

public class Program
  public static method Reset(a: Account): Account => a with(Secret := 0);
end Program;
