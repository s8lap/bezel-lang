//! case: s19_6_0007
//! doc: spec
//! section: 19.6
//! title: a wildcard arm that is not the last arm is rejected
//! rule: If `_` appears, it must be the last arm; placing it earlier is a
//!   compile error.
//! expect: reject
//! diagnostic: last
module Conf;

public class Program
  public static method Label(n: int): string => (case n of
    _ => "other",
    1 => "one");
end Program;
