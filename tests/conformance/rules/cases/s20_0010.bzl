//! case: s20_0010
//! doc: spec
//! section: 20
//! title: a branch needing target-typing from context, like a collection literal, is rejected in an if
//! rule: "**No branch-level target-typing** beyond the bare-`null` rule: a branch
//!   whose form requires a target type from context — a collection literal
//!   (§15.4) ... — is a compile error inside an `if` expression even when the
//!   whole expression occupies a target-typed position."
//! cites: 20, 15.4
//! expect: reject
//! diagnostic: target type
module Conf;

public class Program
  public static method Main()
    var existing: Array<int> := [1, 2, 3];
    var v := (if true then [4, 5, 6] else existing);
  end Main;
end Program;
