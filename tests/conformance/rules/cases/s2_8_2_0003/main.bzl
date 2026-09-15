//! case: s2_8_2_0003
//! doc: spec
//! section: 2.8.2
//! title: a default argument value crosses the artifact boundary
//! rule: The artifact interface must be sufficient for a consumer to
//!   type-check every well-formed use of the module's public surface, with
//!   full signatures — including default argument values.
//! cites: 2.8.2, 22.9
//! expect: run
//! stdout: hello world
module App;
import Lib;

public class Program
  public static method Main()
    Console.WriteLine(Greeter.Greet());
  end Main;
end Program;
