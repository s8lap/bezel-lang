//! case: s11_2_0005
//! doc: spec
//! section: 11.2
//! title: owned must match exactly between an interface parameter and its implementation
//! rule: An implementing method's signature and the interface method's
//!   signature must agree on owned as a parameter modifier. Interface
//!   declaring M(owned t: T) requires implementation to declare M(owned t: T).
//! cites: 11.2
//! expect: reject
//! diagnostic: owned
module Conf;

public class Envelope
  public constructor()
  end constructor;
end Envelope;

public interface IMailbox
  method Send(owned item: Envelope);
end IMailbox;

public class Mailbox: IMailbox
  public method Send(item: Envelope)
  end Send;
end Mailbox;
