//! case: s9_7_0029
//! doc: spec
//! section: 9.7
//! title: an IReadOnlySpan value converted to an Object local is rejected
//! rule: The conversion — §8.6's box, or the reference conversion of an
//!   `IReadOnlySpan<TElement>` value to `Object` or another interface type,
//!   implicit or by an `as` (§17.4) or a typed-pattern binding (§19.3) at
//!   such a type — is a compile error at the conversion site
//! cites: 9.7, 8.6
//! expect: reject
//! diagnostic: borrow-erasing
//! issue: 5659
module Conf;

public class Client
  public static method Run(s: IReadOnlySpan<int>): int
    var o: Object := s;
    return s.Length;
  end Run;
end Client;
