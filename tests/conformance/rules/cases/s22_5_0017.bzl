//! case: s22_5_0017
//! doc: spec
//! section: 22.5
//! title: a call's argument expression runs before the callee's range check raises
//! rule: Evaluation order. A call's supplied argument expressions are evaluated
//!   in call-site textual order - left to right as written, for positional and
//!   named arguments alike; naming an argument changes which parameter it
//!   binds to, never when it runs. The order is observable through
//!   side-effecting arguments and non-constant defaults (DateTime.Now, factory
//!   calls) and is normative. The argument is evaluated before the call
//!   begins - §28.3 reads the same boundary as "not yet transferred because
//!   the call to Combine never began" - so the argument's side effect runs
//!   before the callee's own range check, and stdlib §9.6's contract for the
//!   member is what raises: totalWidth < 0 raises
//!   ArgumentOutOfRangeException.
//! cites: 22.5, 28.3, stdlib 9.6
//! expect: run
//! output: ARG-EVAL
//!   AOORE
module Conf;

public class Program
  private static method Bump(): int
    Console.WriteLine("ARG-EVAL");
    return -1;
  end Bump;

  public static method Main(): int
    try
      var s := ("a" + "b").PadLeft(Program.Bump());
      Console.WriteLine("AFTER");
    catch ArgumentOutOfRangeException =>
      Console.WriteLine("AOORE");
    end try;
    return 0;
  end Main;
end Program;
