//! case: s33_2_0006
//! doc: spec
//! section: 33.2
//! title: async Main(args): Task qualifies
//! rule: `public static async method Main(args: Array<string>): Task` is one
//!   of the eight accepted entry-point signatures.
//! cites: 33.1, C.12
//! expect: run
//! stdout: sig5
module Conf;

public class Program
  public static async method Main(args: Array<string>): Task
    await Task.Delay(0);
    Console.WriteLine("sig5");
  end Main;
end Program;
