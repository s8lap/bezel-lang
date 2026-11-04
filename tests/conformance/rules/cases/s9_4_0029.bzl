//! case: s9_4_0029
//! doc: spec
//! section: 9.4
//! title: A record method that stores the bare instance property is rejected
//! rule: Consequently a store that would land in the **receiver's own storage** is a
//!   compile error at the member body
//! cites: 9.4
//! expect: reject
//! diagnostic: receiver's own storage
//! issue: 8089
//! notes: twin of s9_4_0025 (pairs the rejected with its accepting form).
module Conf;

public record Gauge
  public static Count: int := 0;
  public static property SP: int;
  public property Raw: int;
  public constructor(raw: int)
    Raw := raw;
  end constructor;
  public method M()
    Raw := 100;
  end M;
end Gauge;

public class Program
  public static method Main()
    var g := new Gauge(2);
    g.M();
    Console.WriteLine(g.Raw);
  end Main;
end Program;
