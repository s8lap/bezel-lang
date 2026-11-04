//! case: s9_4_0043
//! doc: spec
//! section: 9.4
//! title: an increment and a store through an interface-typed member of a record are legal
//! rule: a chain that hops through a class- or interface-typed member leaves the receiver
//! cites: 9.7, 11.2
//! expect: run
//! output: 2
//! issue: 8100
module Conf;

public interface ISink
  property Count: int;
end ISink;

public class Sk: ISink
  public property Count: int;

  public constructor()
    Count := 0;
  end constructor;
end Sk;

public record Outer
  public S: ISink;

  public constructor(s: ISink)
    S := s;
  end constructor;

  public method M()
    S.Count++;
    S.Count := S.Count + 1;
  end M;
end Outer;

public class Program
  public static method Main()
    using var s := new Sk();
    var o := new Outer(s);
    o.M();
    Console.WriteLine(s.Count);
  end Main;
end Program;
