//! case: s30_14_0005
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn rejects a non-sendable class-typed argument
//! rule: **Argument and result types must be sendable**, per the rules
//!   above. A **class** type is sendable iff it is transfer-closed: the
//!   class must be `sealed` ... or `closed`. Open classes are not
//!   sendable.
//! cites: 30.14
//! expect: reject
//! diagnostic: sendable
module Conf;

import System.Threading;

public class Payload
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
