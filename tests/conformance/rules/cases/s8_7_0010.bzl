//! case: s8_7_0010
//! doc: spec
//! section: 8.7
//! title: a method reference to Dispose over an enum value is rejected
//! rule: The same no-member outcome holds on a record, enum, choice, primitive
//!   or inline-array value, the value kinds the first paragraph lists, none of
//!   which has a `Dispose()` member (…): `e.Dispose()` and the method reference
//!   `e.Dispose` over an enum value `e` are compile errors (§26.5 rule 2's
//!   no-member outcome).
//! cites: 8.7, 26.5, 27.6
//! notes: §27.6's rule that no method reference to `Dispose` is a form rejects this independently of §8.7's no-member outcome, so the case cannot tell which rule fired. s8_7_0009 isolates §8.7 with the call form.
//! expect: reject
//! diagnostic: Dispose
//! issue: 7930
module Conf;

public enum Color
  Red, Green;
end Color;

public class Program
  public static method Close(e: Color)
    var f := e.Dispose;
  end Close;
end Program;
