//! case: s19_4_0033
//! doc: spec
//! section: 19.4
//! title: an is-test in a when guard narrows that arm's body
//! rule: a successful `is`/typed-pattern test (§12.7, §19.3) — in ... a `when` guard (§19.4, narrowing that arm's body) (v0.338 §7.4 line 900); a `when` guard's arm (v0.338 §12.7 line 2132)
//! cites: 7.4, 12.7
//! expect: run
//! output: 9
//! issue: 6082
module Conf;

public class A
  public constructor() end constructor;
  public x: int := 1;
end A;

public class B: A
  public constructor() end constructor;
  public y: int := 9;
end B;

public class Program
  public static method Probe(a: A)
    case a of
      var q when q is B => do var b: B := q; Console.WriteLine(b.y); end,
      var u => Console.WriteLine("none")
    end case;
  end Probe;

  public static method Main()
    Probe(new B());
  end Main;
end Program;
