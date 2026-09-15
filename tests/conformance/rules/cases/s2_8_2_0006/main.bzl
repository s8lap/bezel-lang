//! case: s2_8_2_0006
//! doc: spec
//! section: 2.8.2
//! title: a contract record carried in the interface is enforced at the consumer
//! rule: For every member the interface describes, it carries one contract
//!   record per contract clause — kind, `static` qualifier, canonical clause
//!   text, recorded tier; a `requires` clause runs in every build mode, so an
//!   imported member's precondition is enforced at the consumer's call site.
//! cites: 2.8.2, 22.16, C.13
//! expect: run
//! stdout: violated
module App;
import Lib;

public class Program
  public static method Main()
    try
      Console.WriteLine(Halver.Of(-4));
    catch
      ContractViolationException => Console.WriteLine("violated"),
    end try;
  end Main;
end Program;
