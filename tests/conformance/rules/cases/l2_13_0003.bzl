//! case: l2_13_0003
//! doc: stdlib
//! section: 2.13
//! title: a keyword-aliased type renders its lowercase keyword in FullName
//! rule: The keyword-aliased types render as their lowercase keyword in both
//!   Name and FullName ("typeof(int).FullName = \"int\""), a recorded
//!   exception to qualification.
//! cites: 2.13
//! expect: run
//! stdout: int
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(typeof(int).FullName);
  end Main;
end Program;
