//! case: s35_3_0013
//! doc: spec
//! section: 35.3
//! title: a same-named member does not win over a case in a declared-result position and the compiler warns
//! rule: "Within a declared-result position whose type *is* such a choice, the case reading is **authoritative**: it wins over any same-named local, parameter, member, or type in scope, and the compiler warns on such shadowing" (§35.3, v0.340). The member `Ok` would return `Error("member")`; the case `Ok(1)` is constructed instead, so the match prints `case 1`.
//! cites: 35.5
//! expect: run
//! warns: yes
//! output: case 1
module Conf;

public class Program
  public static method Ok(v: int): Result<int, string> => Result<int, string>.Error("member");
  public static method P(): Result<int, string> => Ok(1);

  public static method Main()
    Console.WriteLine((case P() of Ok(var v) => $"case {v}", Error(var e) => e));
  end Main;
end Program;
