//! case: s17_6_0001
//! doc: spec
//! section: 17.6
//! title: postfix ++ used as an expression is rejected
//! rule: "`++` and `--` are postfix-only. They are statements, not
//!   expressions ... `x := y++` is a compile error."
//! cites: 17.6, 17.1
//! expect: reject
//! diagnostic: statement
module Conf;

public class Program
  public static method Compute(): int
    var y := 5;
    var x := y++;
    return x;
  end Compute;
end Program;
