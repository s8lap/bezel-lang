//! case: s35_2_0006
//! doc: spec
//! section: 35.2
//! title: a Result combinator is rejected at an owning receiver instantiation
//! rule: Map, MapError, AndThen, and UnwrapOr are admissible only where the receiver
//!   instantiation and the member's result instantiation are both value-like
//!   (§35.4 rule 3); the rejection lands at the use site under §3.6 rule 2, the
//!   diagnostic naming the classification.
//! cites: 35.2, 35.4, 3.6, 14.2
//! expect: reject
//! diagnostic: value-like
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method Main()
    using var fallback := new Connection();
    using var r := Result<Connection, string>.Error("failed");
    var c := r.UnwrapOr(fallback);   // ERROR: Result<Connection, string> is owning, not value-like
  end Main;
end Program;
