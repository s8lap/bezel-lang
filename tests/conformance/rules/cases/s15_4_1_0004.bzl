//! case: s15_4_1_0004
//! doc: spec
//! section: 15.4.1
//! title: a sequence literal targeting a class that is not a sequence-literal target is rejected
//! rule: A sequence literal in a context targeting a type that is not a
//!   sequence-literal target is the usual "cannot infer target type" / "no
//!   applicable target" compile error (§15.4.1).
//! cites: 15.4.1, 15.4
//! expect: reject
//! diagnostic: target
module Conf;

public class Bag
  public constructor()
  end constructor;

  public property Count: int => 0;
end Bag;

public class Program
  public static method Run()
    using var b: Bag := [1, 2, 3];
    Console.WriteLine(b.Count);
  end Run;
end Program;
