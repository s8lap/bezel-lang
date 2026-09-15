//! case: s2_8_4_0003
//! doc: spec
//! section: 2.8.4
//! title: a non-generic type has one static state across three artifacts
//! rule: A process contains exactly one copy of each module's code and, for
//!   each type, exactly one static state and one runtime identity, regardless
//!   of link mode and regardless of how many compilations emitted code for
//!   the type.
//! cites: 2.8.4, 22.12
//! expect: run
//! output: 1
//!   2
//!   3
module App;
import Common;
import DepA;
import DepB;

public class Program
  public static method Main()
    Console.WriteLine(CallerA.Ping());
    Console.WriteLine(CallerB.Ping());
    Console.WriteLine(Counter.Bump());
  end Main;
end Program;
