//! case: s31_4_0005
//! doc: spec
//! section: 31.4
//! title: the (message, cause) constructor snapshots the cause into CauseChain
//! rule: The `(message, cause)` constructor borrows `cause` for the duration
//!   of the call and copies its type name, message, and stack-trace text into
//!   the new exception's `CauseChain`.
//! cites: 31.4
//! expect: run
//! stdout: inner
module Conf;

public class CauseException: Exception
  public constructor(message: string) base(message)
  end constructor;
end CauseException;

public class WrapException: Exception
  public constructor(message: string, cause: Exception) base(message, cause)
  end constructor;
end WrapException;

public class Program
  public static method Main()
    using var wrap := new WrapException("outer", new CauseException("inner"));
    Console.WriteLine(wrap.CauseChain[0].Message);
  end Main;
end Program;
