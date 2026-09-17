//! case: s33_2_0010
//! doc: spec
//! section: 33.2
//! title: an instance Main overload does not conflict with a qualifying one
//! rule: An instance method named `Main` does not qualify. (A class may still
//!   declare a non-qualifying `Main` alongside a qualifying one.)
//! cites: 33.1, 33.5
//! expect: run
//! stdout: qualifying-ran
module Conf;

public class Program
  public static method Main(args: Array<string>)
    Console.WriteLine("qualifying-ran");
  end Main;

  public method Main()
    Console.WriteLine("never-called");
  end Main;
end Program;
