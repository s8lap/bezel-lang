//! case: l1_2303
//! doc: stdlib
//! section: 1
//! title: Count() on an IReadOnlyCollection parameter is rejected: the
//!   inherited property blocks the operator
//! rule: wherever a type of this document declares or inherits an instance
//!   member named `Count`, `Contains`, `First`, `Last`, `Reverse`, or
//!   `ToArray`, a call on that receiver resolves to the member and the
//!   same-named
//! cites: 1, 4.1, 26.5
//! expect: reject
//! diagnostic: is a property of
module Conf;

public class Program
  public static method N(c: IReadOnlyCollection<int>): int
    return c.Count();
  end N;
end Program;
