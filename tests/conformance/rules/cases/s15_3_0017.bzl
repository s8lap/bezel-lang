//! case: s15_3_0017
//! doc: spec
//! section: 15.3
//! title: the same generic record named at a copyable argument is admitted
//! rule: the ban reads a component's declared type after substitution (§3.6
//!   rule 2): a record component, positional or body, whose declared type is
//!   or contains a type parameter is rejected at every instantiation at which
//!   that type is a move-only value type
//! cites: 15.3, 3.6
//! expect: accept
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public choice Slot
  Full(c: int),
  Empty;
end Slot;

public record Box<T>(V: T);

public class User
  public static method Run(b: Box<Slot>): int => 1;
end User;
