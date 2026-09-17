//! case: s30_14_0006
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn admits a sealed class-typed argument
//! rule: A **class** type is sendable iff it is transfer-closed: the class
//!   is `sealed`; or it is `closed` and every class in its hierarchy is
//!   itself transfer-closed.
//! cites: 30.14
//! expect: accept
module Conf;

import System.Threading;

public sealed class Payload
  public constructor()
  end constructor;
end Payload;

public class Program
  public static method Handle(owned p: Payload)
  end Handle;

  public method Run()
    using var t := Domain.Spawn<Payload>(Program.Handle, new Payload());
  end Run;
end Program;
