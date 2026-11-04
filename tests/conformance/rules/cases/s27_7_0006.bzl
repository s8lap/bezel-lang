//! case: s27_7_0006
//! doc: spec
//! section: 27.7
//! title: a nullable function-typed property is invoked through the null-conditional call spelling
//! rule: at an invocation whose callee is a function-typed value — a parameter, local, field, or property of function type, or any expression of one
//! cites: 27.7, 27.2, 7.7
//! expect: run
//! issue: 7543
//! stdout: 5

module Conf;

public class Foo
  public property F: ((int) -> int)? := lambda(x: int) => x + 1;
end Foo;

public class Program
  public static method Main()
    var o := new Foo();
    Console.WriteLine((o.F?(4) ?? -1).ToString());
  end Main;
end Program;
