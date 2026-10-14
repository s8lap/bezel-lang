//! case: s19_4_0034
//! doc: spec
//! section: 19.4
//! title: a null test in one catch arm's when guard narrows that arm's body and not a sibling catch arm
//! rule: or a `when` guard (§19.4, narrowing that arm's body) (v0.338 §7.4 line 900); The arms are tried in order; the first arm whose pattern matches the thrown exception (and whose guard, if any, evaluates to `true`) is selected, its body runs, and exception handling completes. Arms after the matched one are not considered. (v0.338 §31.2)
//! cites: 31.2, 7.4
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
    try
      throw new IOException("io");
    catch
      IOException when a <> null => Console.WriteLine("one"),
      Exception => Console.WriteLine(a.x)
    end try;
  end Probe;

  public static method Main()
    Probe(new B());
  end Main;
end Program;
