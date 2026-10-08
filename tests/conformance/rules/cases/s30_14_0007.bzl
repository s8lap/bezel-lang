//! case: s30_14_0007
//! doc: spec
//! section: 30.14
//! title: a sealed class deriving from Exception is not sendable as a spawn argument
//! rule: The inherited state of a **compiler-known** class among the class's
//!   bases, direct or indirect (§12.4's opened classes), is not read as
//!   fields (stdlib §1): it satisfies this rule only where the base's own
//!   section states its inherited state transfer-compatible, and fails it
//!   otherwise.
//! cites: 30.14, 12.4, 31.4
//! expect: reject
//! diagnostic: not sendable
//! issue: 5637
module Conf;

import System.Threading;

public sealed class Fault: Exception
  public Detail: string := "disk";
end Fault;

public class Program
  public static method Handle(owned f: Fault)
  end Handle;

  public method Run()
    using var t := Domain.Spawn<Fault>(Program.Handle, new Fault());
  end Run;
end Program;
