//! case: l1_2304
//! doc: stdlib
//! section: 1
//! title: Count() on a receiver bounded by IList is rejected: the inherited
//!   property blocks the operator
//! rule: wherever a type of this document declares or inherits an instance
//!   member named `Count`, `Contains`, `First`, `Last`, `Reverse`, or
//!   `ToArray`, a call on that receiver resolves to the member and the
//!   same-named
//! cites: 1, 4.1, 26.5
//! expect: reject
//! diagnostic: is a property of
module Conf;

public class Program
  public static method N<T>(t: T): int where T: IList<int>
    return t.Count();
  end N;
end Program;
