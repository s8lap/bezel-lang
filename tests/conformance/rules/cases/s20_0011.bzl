//! case: s20_0011
//! doc: spec
//! section: 20
//! title: a bare case construction inside an if branch resolves ordinarily, not as case construction
//! rule: "not to subexpression positions within a return operand — in `return (if
//!   c then Ok(v) else Error(e));` the branches are operands of the `(if …)` atom
//!   (§20) and resolve ordinarily; write two `return` statements, or the
//!   qualified form."
//! cites: 20, 35.3
//! expect: reject
//! diagnostic: resolve ordinarily
module Conf;

public class Program
  public static method Parse(text: string): Result<int, string>
    var value: int := 0;
    return (if int.TryParse(text, out value) then Ok(value) else Error("bad"));
  end Parse;

  public static method Main()
    var r := Parse("5");
  end Main;
end Program;
