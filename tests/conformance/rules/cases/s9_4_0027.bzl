//! case: s9_4_0027
//! doc: spec
//! section: 9.4
//! title: A lambda inside a record setter that stores to a static field is legal
//! rule: The receiver's own storage is what an lvalue chain rooted at `this` reaches
//!   without leaving the value
//! cites: 9.4
//! expect: run
//! stdout: 3
//! issue: 8089
//! notes: twin of s9_4_0031 (pairs the accepted with its rejecting form).
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
    set => do var f := lambda() => do Count := Count + value; end; f(); end;
  end D;
end Gauge;

public class Program
  public static method Main()
    var g := new Gauge(2);
    g.D := 3;
    Console.WriteLine(Gauge.Count);
  end Main;
end Program;
