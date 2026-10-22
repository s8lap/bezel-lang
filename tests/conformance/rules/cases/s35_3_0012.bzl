//! case: s35_3_0012
//! doc: spec
//! section: 35.3
//! title: a bare case construction in a declared-result position binds its argument against the payload type
//! rule: "Within a declared-result position whose type *is* such a choice, the case reading is **authoritative**" and "Payload arguments bind to the case's payload fields as to a parameter list" (§35.3, v0.340). The member `Program.Got(a: int)` does not win, and the `int` argument does not convert to the `string` payload `body`.
//! cites: 35.5
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Program
  public static method Got(a: int): Fetch => Fetch.NotModified;
  public static method Use(): Fetch => Got(5);
  public static method Main() end Main;
end Program;
