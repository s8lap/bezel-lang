//! case: s27_6_3601
//! doc: spec
//! section: 27.6
//! title: a type-qualified reference to an instance method names no member
//! rule: over an instance **method** of `T` it names no member, there being no receiver, and is §26.5's
//!   compile error at the name
//! cites: 27.6, 26.5
//! expect: reject
//! diagnostic: Foo
//! issue: 8065
//! notes: The receiver-taking function is written `lambda(k: K) => k.Foo()`. The binding carries no
//!   annotation, so no target-type mismatch can stand in for the error at the name.
module Conf;

public class K
  public constructor() end constructor;
  public method Foo(): int => 1;
end K;

public class Program
  public static method Main()
    var f := K.Foo;
    using var k := new K();
    Console.WriteLine(f(k));
  end Main;
end Program;
