//! case: s9_4_0031
//! doc: spec
//! section: 9.4
//! title: A lambda inside a record setter that stores the instance property is rejected
//! rule: a *setter body* whose store lands in the receiver's own storage is rejected
//!   like any other member body
//! cites: 9.4
//! expect: reject
//! diagnostic: receiver's own storage
//! issue: 8089
//! notes: twin of s9_4_0027 (pairs the rejected with its accepting form).
module Conf;

public record Gauge
  public static Count: int := 0;
  public static property SP: int;
  public property Raw: int;
  public constructor(raw: int)
    Raw := raw;
  end constructor;
  public property D: int
    get => Raw;
    set => do var f := lambda() => do Raw := value; end; f(); end;
  end D;
end Gauge;

public class Program
  public static method Main()
    var g := new Gauge(2);
    g.D := 3;
    Console.WriteLine(g.Raw);
  end Main;
end Program;
