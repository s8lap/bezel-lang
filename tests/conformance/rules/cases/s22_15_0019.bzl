//! case: s22_15_0019
//! doc: spec
//! section: 22.15
//! title: the throw carve-out is rejected when the exception class is not trivial-dispose
//! rule: `new E(args)` is admitted [in a throw operand] only when ... E
//!   is a trivial-dispose class.
//! cites: 22.15, 28.9
//! expect: reject
//! diagnostic: trivial-dispose
module Conf;

public class HeavyError: Exception
  public pure constructor(message: string) base(message)
  end constructor;

  destructor
  end destructor;
end HeavyError;

public class Ops
  public static pure method Fail(): int
    throw new HeavyError("boom");
  end Fail;
end Ops;
