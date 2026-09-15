//! case: s3_6_0001
//! doc: spec
//! section: 3.6
//! title: default(int) is defined as the zero value
//! rule: defaultable classifies types for which default(T) is defined: every
//!   non-nullable value type whose components are recursively defaultable —
//!   the primitives among them.
//! cites: 3.6, 17.11
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{default(int)}");
  end Main;
end Program;
