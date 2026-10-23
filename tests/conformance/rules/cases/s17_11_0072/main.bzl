//! case: s17_11_0072
//! doc: spec
//! section: 17.11
//! title: an external instantiation whose library body takes no default(T) admits a non-defaultable argument
//! rule: For unconstrained `T`, `default(T)` is permitted and has type `T`:
//!   the rule below rejects at the instantiation site every
//!   substitution that is not **defaultable** — the obligation is the
//!   generic body's own use of `default(T)`, so a generic whose body
//!   takes none carries no defaultability obligation on its type
//!   parameter.
//!   An external instantiation behaves identically to a same-module
//!   instantiation of the same generic at the same arguments (§2.8.2).
//! cites: 17.11, 3.6, 2.8.2
//! expect: run
//! stdout: 5
module Conf;
import L;

public class Program
  public static method Main(): int
    Console.WriteLine(U.Plain<string>());
    return 0;
  end Main;
end Program;
