//! case: s17_11_0021
//! doc: spec
//! section: 17.11
//! title: an unconstrained default(T) is rejected where the method is called at string
//! rule: For unconstrained `T`, `default(T)` is permitted and has type `T`: the
//!   rule below rejects at the instantiation site every substitution that is
//!   not **defaultable** — a non-nullable reference type (a class, an
//!   interface, `Object`, `string`, a function type, a task type), ...
//! cites: 17.11, 3.6
//! expect: reject
//! diagnostic: defaultable
//! issue: 5820
module Conf;

public class Zero
  public static method Count<T>(): int
    var d: T := default(T);
    return 1;
  end Count;

  public static method Run(): int => Zero.Count<string>();
end Zero;
