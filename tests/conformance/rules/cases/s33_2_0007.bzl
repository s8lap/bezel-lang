//! case: s33_2_0007
//! doc: spec
//! section: 33.2
//! title: async Main(): Task<int> qualifies
//! rule: `public static async method Main(): Task<int>` is one of the eight
//!   accepted entry-point signatures.
//! cites: 33.1, 33.3, C.12
//! expect: run
//! stdout: sig6
module Conf;

public class Program
  public static async method Main(): Task<int>
    await Task.Delay(0);
    Console.WriteLine("sig6");
    return 0;
  end Main;
end Program;
