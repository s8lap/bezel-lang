//! case: s17_14_0010
//! doc: spec
//! section: 17.14
//! title: a local const with a non-constant-expression initializer is a runtime const, not an error
//! rule: "A *local* `const` failing either condition is a **runtime const**
//!   rather than an error; a reference to a local `const` is a constant
//!   expression only when the declaration is compile-time-classified."
//! cites: 17.14, 21.1
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    const z := default(int);
    Console.WriteLine(z);
  end Main;
end Program;
