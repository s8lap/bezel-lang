//! case: s2_8_4_0001
//! doc: spec
//! section: 2.8.4
//! title: a generic instantiation's static state is one copy across
//!   independently compiled artifacts
//! rule: A process contains exactly one copy of each module's code and, for
//!   each type — including each generic instantiation — exactly one static
//!   state and one runtime identity, regardless of link mode and regardless
//!   of how many compilations emitted code for the type.
//! cites: 2.8.4, 2.8.3
//! expect: run
//! output: 1
//!   2
//!   3
module Conf;
import Common;
import DepA;
import DepB;

public class Program
  public static method Main()
    Console.WriteLine(CallerA.Ping());
    Console.WriteLine(CallerB.Ping());
    Console.WriteLine(Box<int>.Bump());
  end Main;
end Program;
