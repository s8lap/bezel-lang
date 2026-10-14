//! case: s19_4_0032
//! doc: spec
//! section: 19.4
//! title: a null test in one arm's when guard narrows that arm's body and not a sibling arm
//! rule: or a `when` guard (§19.4, narrowing that arm's body) (v0.338 §7.4 line 900); Arms are evaluated in textual order. The first arm whose pattern matches *and* whose guard is true is selected. (v0.338 §19.4 line 3990)
//! cites: 7.4, 12.7
//! expect: reject
//! diagnostic: nullable
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
  public static method Probe(a: A?)
    case a of
      var q when a <> null => Console.WriteLine("one"),
      var u => Console.WriteLine(a.x)
    end case;
  end Probe;

  public static method Main()
    Probe(new B());
  end Main;
end Program;
