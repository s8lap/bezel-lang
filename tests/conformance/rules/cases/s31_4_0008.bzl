//! case: s31_4_0008
//! doc: spec
//! section: 31.4
//! title: SupersededChain is empty on an exception that superseded nothing
//! rule: `SupersededChain` (v0.259 — empty when nothing superseded). It starts
//!   empty and shares the empty-array representation, so an exception that
//!   supersedes nothing allocates nothing.
//! cites: 31.4, 31.2
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    try
      throw new ArgumentException("boom");
    catch
      var ex: Exception => Console.WriteLine(ex.SupersededChain.Count),
    end try;
  end Main;
end Program;
