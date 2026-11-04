//! case: s9_4_0026
//! doc: spec
//! section: 9.4
//! title: A lambda in a record constructor that stores to a static field is legal
//! rule: The receiver's own storage is what an lvalue chain rooted at `this` reaches
//!   without leaving the value
//! cites: 9.4, 22.8
//! expect: run
//! stdout: 6
//! issue: 8089
//! notes: twin of s9_4_0030 (pairs the accepted with its rejecting form).
module Conf;

public record Gauge
  public static Count: int := 0;
  public static property SP: int;
  public property Raw: int;
  public constructor(raw: int)
    Raw := raw;
  end constructor;
  public constructor(raw: int, k: int)
    Raw := raw;
    var f := lambda() => do Count := Count + k; end;
    f();
  end constructor;
end Gauge;

public class Program
  public static method Main()
    var g := new Gauge(2, 6);
    Console.WriteLine(Gauge.Count);
  end Main;
end Program;
