//! case: s7_5_0025
//! doc: spec
//! section: 7.5
//! title: parentheses end the ?. chain, so a member access on the int? is rejected
//! rule: Parentheses end the chain: `(s?.Length).ToString()` is a member access
//!   on an `int?`, §7.4's error.
//! cites: 7.5, 7.4
//! expect: reject
//! diagnostic: nullable
//! issue: 6070
module Conf;

public class Program
  public static method Render(s: string?): string
    return (s?.Length).ToString();
  end Render;
end Program;
