//! case: s3_4_0005
//! doc: spec
//! section: 3.4
//! title: a user class named like a library collection keeps its own static member
//! rule: This does not reserve other standard-library type names — only the
//!   PascalCase aliases of the primitive value types and `String`; an ordinary
//!   library type such as `Array` or `Stream` may still be shadowed by a user type
//!   under the normal name-resolution rules.
//! cites: 3.4, 2.7
//! expect: run
//! stdout: 3
module Conf;

public class Stack
  public static method Foo(): int
    return 3;
  end Foo;
end Stack;

public class Program
  public static method Main()
    Console.WriteLine($"{Stack.Foo()}");
  end Main;
end Program;
