//! case: s33_2_0005
//! doc: spec
//! section: 33.2
//! title: async Main(): Task qualifies
//! rule: `public static async method Main(): Task` is one of the eight
//!   accepted entry-point signatures.
//! cites: 33.1, C.12
//! expect: run
//! stdout: sig4
module Conf;

public class Program
  public static async method Main(): Task
    await Task.Delay(0);
    Console.WriteLine("sig4");
  end Main;
end Program;
