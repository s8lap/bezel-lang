//! case: s9_4_0030
//! doc: spec
//! section: 9.4
//! title: A lambda in a record constructor that stores the instance property is rejected
//! rule: **Constructor bodies are not receiver positions**
//! cites: 9.4, 22.8
//! expect: reject
//! diagnostic: receiver's own storage
//! issue: 8089
//! notes: twin of s9_4_0026 (pairs the rejected with its accepting form).
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
    var f := lambda() => do Raw := k; end;
    f();
  end constructor;
end Gauge;

public class Program
  public static method Main()
    var g := new Gauge(2, 6);
    Console.WriteLine(g.Raw);
  end Main;
end Program;
