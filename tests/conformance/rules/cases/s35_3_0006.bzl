//! case: s35_3_0006
//! doc: spec
//! section: 35.3
//! title: the declared-result case reading does not extend into a subexpression position
//! rule: The case reading takes exactly the declared-result group and does not extend
//!   beyond it — not to subexpression positions within a return operand: in
//!   return (if c then Ok(v) else Error(e)); the branches are operands of the
//!   (if) atom and resolve ordinarily.
//! expect: reject
//! diagnostic: qualify
module Conf;

public class Program
  public static method Choose(c: bool): Result<int, string>
    return (if c then Ok(4) else Error("no"));   // ERROR: `Ok`/`Error` are not declared-result positions here
  end Choose;

  public static method Main()
    Console.WriteLine(1);
  end Main;
end Program;
