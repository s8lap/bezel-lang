//! case: s35_2_0006
//! doc: spec
//! section: 35.2
//! title: Result.Map is rejected at an owning receiver instantiation
//! rule: Map, MapError, AndThen, and UnwrapOr are admissible only where the receiver
//!   instantiation and the member's result instantiation are both value-like
//!   (§35.4 rule 3); a call at a borrow-bearing or owning instantiation is
//!   rejected at the use site.
//! cites: 35.2, 35.4, 3.6
//! expect: reject
//! diagnostic: value-like
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method Main()
    using var r := Result<Connection, string>.Error("failed");
    var mapped := r.Map(lambda(c: Connection) => c);   // ERROR: owning instantiation is not value-like
  end Main;
end Program;
