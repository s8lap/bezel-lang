//! case: s8_7_0009
//! doc: spec
//! section: 8.7
//! title: a Dispose() call on an enum value is rejected
//! rule: The same no-member outcome holds on a record, enum, choice, primitive
//!   or inline-array value, the value kinds the first paragraph lists, none of
//!   which has a `Dispose()` member: `e.Dispose()` and the method reference
//!   `e.Dispose` over an enum value `e` are compile errors (§26.5 rule 2's
//!   no-member outcome).
//! cites: 8.7, 26.5
//! expect: reject
//! diagnostic: Dispose
//! issue: 7930
module Conf;

public enum Color
  Red, Green;
end Color;

public class Program
  public static method Close(e: Color)
    e.Dispose();
  end Close;
end Program;
