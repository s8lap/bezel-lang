//! case: s3_1_0002
//! doc: spec
//! section: 3.1
//! title: an interface variable may hold a boxed int value
//! rule: An interface variable is a reference; its underlying value may be a class
//!   instance, a boxed record, a boxed enum, or a boxed primitive.
//! expect: run
//! stdout: 42
//! cites: 3.1
module Test;

import System;

public class Test
  public static method Main()
    var x: IComparable<int> := 42;
    Console.WriteLine(x);
  end Main;
end Test;
