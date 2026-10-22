//! case: s35_3_0011
//! doc: spec
//! section: 35.3
//! title: a bare case construction whose argument count differs from the payload is the arity error
//! rule: "Within a declared-result position whose type *is* such a choice, the case reading is **authoritative**: it wins over any same-named local, parameter, member, or type in scope" (§35.3, v0.340). The member `Program.Got(a, b)` does not win over the case `Got(body)`, so `return Got(1, 2);` is a case construction that supplies 2 arguments for a one-field payload, an arity error.
//! cites: 35.5
//! expect: reject
//! diagnostic: supplies 2 argument
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Program
  public static method Got(a: int, b: int): Fetch => Fetch.NotModified;

  public static method Use(): Fetch
    return Got(1, 2);
  end Use;

  public static method Main() end Main;
end Program;
