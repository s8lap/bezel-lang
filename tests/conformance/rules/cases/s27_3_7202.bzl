//! case: s27_3_7202
//! doc: spec
//! section: 27.3
//! title: a lambda with an inferred parameter at a const function-typed
//!   local takes the target type
//! rule: the initializer of a declared function-typed local or field, an
//!   assignment to a function-typed location, a `return` against a declared
//!   function-typed return type — is admitted under the argument-position
//!   rule
//! cites: 27.3, 27.4
//! expect: run
//! stdout: 8
module Conf;

public class Program
  public static method Main()
    const f: (int) -> int := lambda(x) => x * 2;
    Console.WriteLine(f(4));
  end Main;
end Program;
