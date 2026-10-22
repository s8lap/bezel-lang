//! case: s35_3_0028
//! doc: spec
//! section: 35.3
//! title: a bare case name as a typed assignment target value is not target-typed
//! rule: "The case reading takes exactly the declared-result group ... and does not extend beyond it: ... not to `var` initialization, typed local declarations, field initializers, default parameter values, or typed assignment targets" (§35.3, v0.340); a typed assignment to an existing local is such a position. Outside the family a bare case name keeps its ordinary resolution, and `NotModified` names nothing.
//! cites: 35.5
//! expect: reject
//! diagnostic: NotModified
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Program
  public static method A(): Fetch
    var y: Fetch := Fetch.Got("a");
    y := NotModified;
    return y;
  end A;

  public static method Main() end Main;
end Program;
