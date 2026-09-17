//! case: l2_6_0001
//! doc: stdlib
//! section: 2.6
//! title: Empty equals default(Guid)
//! rule: Guid.Empty is the all-zero UUID -- the same value default(Guid)
//!   produces.
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Guid.Empty = default(Guid));
  end Main;
end Program;
