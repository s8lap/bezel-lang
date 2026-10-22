//! case: s35_3_0014
//! doc: spec
//! section: 35.3
//! title: a local function value named like a case does not win over the case in a declared-result return
//! rule: "the case reading is **authoritative**: it wins over any same-named local, parameter, member, or type in scope, and the compiler warns on such shadowing" (§35.3, v0.340). The local `Got` (a lambda returning `NotModified`) is shadowed, so `return Got("x");` constructs the case with payload `x`.
//! cites: 35.5
//! expect: run
//! warns: yes
//! output: x
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Program
  public static method P(): Fetch
    var Got := lambda(s: string): Fetch => Fetch.NotModified;
    return Got("x");
  end P;

  public static method Main()
    Console.WriteLine((case P() of Got(var b) => b, NotModified => "nm"));
  end Main;
end Program;
