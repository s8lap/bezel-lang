//! case: s27_7_0004
//! doc: spec
//! section: 27.7
//! title: a function-typed property is called with call syntax on its receiver
//! rule: "Calling function values" (§27.7, v0.350): a value of function type is
//!   invoked with ordinary call syntax, `add(2, 3)`. A property whose declared type is
//!   a function type is such a value, so `u.F(4)` invokes the lambda the property
//!   holds with the argument 4 and yields 5.
//! cites: 27.7, 27.1
//! expect: run
//! stdout: 5
module Conf;

import System;

public class U
  public property F: (int) -> int := lambda(x: int) => x + 1;
end U;

public class Program
  public static method Main()
    var u := new U();
    Console.WriteLine(u.F(4));
  end Main;
end Program;
