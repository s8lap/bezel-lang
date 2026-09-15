//! case: s22_12_0003
//! doc: spec
//! section: 22.12
//! title: static const is the redundant-but-permitted spelling of a class-level const
//! rule: A class-level `const` is implicitly static; writing `static
//!   const` is redundant but permitted for clarity — both forms declare
//!   the same constant.
//! cites: 22.12
//! expect: run
//! stdout: 3.5
module Conf;

public class Constants
  public static const Ratio: double := 3.5;
end Constants;

public class Program
  public static method Main()
    Console.WriteLine(Constants.Ratio);
  end Main;
end Program;
