//! case: s10_2_0001
//! doc: spec
//! section: 10.2
//! title: same-valued constants denote one value under =
//! rule: Two or more constants of one enum may share an underlying value.
//!   Same-valued constants denote one value: `Legacy = Current` is `true`,
//!   and no construct can distinguish them at run time.
//! cites: 10.2, 8.3
//! expect: run
//! stdout: True
module Conf;

enum Level
  Legacy := 1, Current := 1, Retired := 2;
end Level;

public class Program
  public static method Main()
    Console.WriteLine(Level.Legacy = Level.Current);
  end Main;
end Program;
