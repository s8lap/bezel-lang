//! case: s17_11_0066
//! doc: spec
//! section: 17.11
//! title: an external instantiation whose library body reaches default(T) through a chain is rejected
//! rule: For unconstrained `T`, `default(T)` is permitted and has type `T`: the
//!   rule below rejects at the instantiation site every substitution that is
//!   not **defaultable** ... read through any chain of user declarations
//!   (§3.6 rule 2), so at every admitted substitution the result is that
//!   type's zero value.
//!   An external instantiation behaves identically to a same-module
//!   instantiation of the same generic at the same arguments (§2.8.2).
//! cites: 17.11, 3.6, 2.8.2
//! expect: reject
//! diagnostic: defaultable
module Conf;
import L;

public class Program
  public static method Main(): int
    Console.WriteLine(U.Wrap<string>());
    return 0;
  end Main;
end Program;
