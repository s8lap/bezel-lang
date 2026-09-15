//! case: s33_2_0008
//! doc: spec
//! section: 33.2
//! title: async Main(args): Task<int> qualifies
//! rule: `public static async method Main(args: Array<string>): Task<int>` is
//!   one of the eight accepted entry-point signatures.
//! cites: 33.1, 33.3, C.12
//! expect: run
//! stdout: sig7
module Conf;

public class Program
  public static async method Main(args: Array<string>): Task<int>
    await Task.Delay(0);
    Console.WriteLine("sig7");
    return 0;
  end Main;
end Program;
