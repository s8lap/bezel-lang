//! case: s31_4_0006
//! doc: spec
//! section: 31.4
//! title: the cause instance is never retained after chaining
//! rule: The cause instance is never retained — consistent with §31.1's
//!   caught-exception-is-a-borrow rule, no ownership machinery is needed, and
//!   the full diagnostic chain survives independently of the cause instance.
//! cites: 31.4, 31.1
//! expect: run
//! stdout: inner
//! flags: --checked-disposal
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
    var cause := new CauseException("inner");
    using var wrap := new WrapException("outer", cause);
    cause.Dispose();
    Console.WriteLine(wrap.CauseChain[0].Message);
  end Main;
end Program;
