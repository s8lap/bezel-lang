//! case: s15_3_0016
//! doc: spec
//! section: 15.3
//! title: a generic record component is rejected where the record is named at an owning choice
//! rule: A move-only value may not be a field of a record ...; the ban reads a
//!   component's declared type after substitution (§3.6 rule 2): a record
//!   component, positional or body, whose declared type is or contains a type
//!   parameter is rejected at every instantiation at which that type is a
//!   move-only value type ... so `record Box<T>(V: T)` is rejected wherever
//!   `Box<[2 owned]Conn?>` or `Box<Slot>`, for an owning choice `Slot`, is named.
//! cites: 15.3, 3.6, 14.1
//! expect: reject
//! diagnostic: move-only
//! issue: 5819
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public choice Slot
  Full(owned c: Conn),
  Empty;
end Slot;

public record Box<T>(V: T);

public class User
  public static method Run(b: Box<Slot>): int => 1;
end User;
