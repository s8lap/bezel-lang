//! case: s35_2_0005
//! doc: spec
//! section: 35.2
//! title: a plain Equals/GetHashCode member is declarable on an owning choice
//! rule: A choice of any classification — owning and borrow-bearing included — may
//!   declare plain Equals(other: C): bool and GetHashCode(): int members, statically
//!   dispatched like every choice method; on a non-boxable classification they
//!   replace nothing and are reachable by static dispatch only.
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public choice AcquireOutcome
  Ready(owned conn: Connection),
  Empty;

  public method Equals(other: AcquireOutcome): bool => (case this of
    Ready(_) => (case other of Ready(_) => true, Empty => false),
    Empty    => (case other of Empty => true, Ready(_) => false));

  public method GetHashCode(): int => (case this of Ready(_) => 1, Empty => 0);
end AcquireOutcome;
