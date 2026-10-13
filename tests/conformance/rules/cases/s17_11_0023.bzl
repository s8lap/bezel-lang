//! case: s17_11_0023
//! doc: spec
//! section: 17.11
//! title: a field initialized by default(T) is rejected where its type is named at a class
//! rule: For unconstrained `T`, `default(T)` is permitted and has type `T`: the
//!   rule below rejects at the instantiation site every substitution that is
//!   not **defaultable** — a non-nullable reference type (a class, ...) ...
//!   read through any chain of user declarations (§3.6 rule 2)
//! cites: 17.11, 3.6, 14.1
//! expect: reject
//! diagnostic: defaultable
//! issue: 5820
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Cell<T>
  public V: T := default(T);

  public constructor()
  end constructor;
end Cell;

public class User
  public static method Run(c: Cell<Widget>): int => 1;
end User;
