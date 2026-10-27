//! case: s17_11_0067
//! doc: spec
//! section: 17.11
//! title: the same chained external instantiation at int runs and yields the zero value
//! rule: For unconstrained `T`, `default(T)` is permitted and has type `T`: the
//!   rule below rejects at the instantiation site every substitution that is
//!   not **defaultable** ... read through any chain of user declarations
//!   (§3.6 rule 2), so at every admitted substitution the result is that
//!   type's zero value.
//!   An external instantiation behaves identically to a same-module
//!   instantiation of the same generic at the same arguments (§2.8.2).
//! cites: 17.11, 3.6, 2.8.2
//! expect: run
//! stdout: 0
module Conf;
import L;

public class Program
  public static method Main(): int
    Console.WriteLine(U.Wrap<int>());
    return 0;
  end Main;
end Program;
