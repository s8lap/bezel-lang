//! case: l5_0001
//! doc: stdlib
//! section: 5
//! title: Where's predicate is not invoked until the query is enumerated
//! rule: All are lazy unless documented otherwise. Callback retention: a
//!   predicate passed to a lazy operator is retained by the returned query
//!   value until enumeration, not invoked at the call.
//! cites: 5
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    var calls := 0;
    using var q := xs.Where(lambda(x: int): bool => do
      calls := calls + 1;
      return x > 1;
    end);
    Console.WriteLine(calls);
  end Main;
end Program;
