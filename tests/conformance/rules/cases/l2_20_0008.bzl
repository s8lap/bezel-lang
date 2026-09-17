//! case: l2_20_0008
//! doc: stdlib
//! section: 2.20
//! title: Map is unreachable at an owning Result instantiation
//! rule: Map/MapError/AndThen/UnwrapOr are conditionally-admitted members,
//!   admissible only where the receiver instantiation is value-like; a
//!   borrow-bearing or owning receiver is rejected at the use site.
//! cites: 2.20, 14.2, 35.4
//! expect: reject
//! diagnostic: value-like
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method MakeError(): Result<Connection, string>
    return Error("nope");
  end MakeError;

  public method Demo()
    var r := MakeError();
    var r2 := r.Map(lambda(c) => 1);
  end Demo;
end Program;
