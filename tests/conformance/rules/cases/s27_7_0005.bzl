//! case: s27_7_0005
//! doc: spec
//! section: 27.7
//! title: a function-typed field is invoked through the member-access spelling
//! rule: at an invocation whose callee is a function-typed value — a parameter, local, field, or property of function type, or any expression of one
//! cites: 27.7, 27.1, 26.5
//! expect: run
//! issue: 7543
//! stdout: 8

module Conf;

public class Foo
  public f: (int) -> int := lambda(x: int) => x + 7;
end Foo;

public class Program
  public static method Main()
    var o := new Foo();
    Console.WriteLine(o.f(1).ToString());
  end Main;
end Program;
