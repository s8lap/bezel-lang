//! case: s28_3_0126
//! doc: spec
//! section: 28.3
//! title: undischarged owned parameters of a method are disposed in reverse declaration order on every edge
//! rule: Undischarged `owned` parameters' implicit disposals are established at
//!   entry to the body, in declaration order — so, undischarged, they are
//!   disposed in reverse declaration order, for a method, a constructor, a
//!   lambda and an uncaptured primary-constructor parameter alike
//! cites: 28.3, 28.10
//! expect: run
//! output: body
//!   b
//!   a
//!   d
//!   c
//!   caught
//! issue: 5808
module Conf;

public class Tag
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine(_name);
  end destructor;
end Tag;

public class Demo
  public static method Use(owned a: Tag, owned b: Tag, fail: bool)
    if fail then
      throw new InvalidOperationException("boom");
    end;
    Console.WriteLine("body");
  end Use;

  public static method Main()
    Use(new Tag("a"), new Tag("b"), false);
    try
      Use(new Tag("c"), new Tag("d"), true);
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Demo;
