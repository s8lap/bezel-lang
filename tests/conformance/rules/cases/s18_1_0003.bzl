//! case: s18_1_0003
//! doc: spec
//! section: 18.1
//! title: a suffix-less over-int literal is admitted at a written integer target whose range contains it
//! rule: The smallest legal edit of s18_1_0002 — ascribing the target type — is
//!   admitted: "(At a written integer target whose range contains it the literal
//!   is admitted at the target type — §4.2's target-typed integer constant.)"
//! cites: 18.1, 4.2
//! expect: accept
module Conf;

public class Program
  private static method Compute()
    var total: long := 3_000_000_000;
  end Compute;
end Program;
