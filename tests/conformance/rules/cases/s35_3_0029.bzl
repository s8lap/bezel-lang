//! case: s35_3_0029
//! doc: spec
//! section: 35.3
//! title: a bare case name in a yield return is not target-typed
//! rule: The declared-result position is "the top-level operand of a `return e;` statement" (§35.3, v0.340); a `yield return` operand is not one, so `NotModified` keeps its ordinary resolution and names nothing.
//! cites: 35.5
//! expect: reject
//! diagnostic: NotModified
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Program
  public static method Gen(): IEnumerable<Fetch>
    yield return NotModified;
  end Gen;

  public static method Main()
    for var f in Gen() do Console.WriteLine((case f of Got(var b) => b, NotModified => "nm")); end;
  end Main;
end Program;
