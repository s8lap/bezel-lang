//! case: s28_9_0012
//! doc: spec
//! section: 28.9
//! title: an owned field of trivial-dispose type leaves the holder trivial-dispose
//! rule: The trivial-dispose check is recursive: an `owned` field whose
//!   declared type is itself trivial-dispose does not disqualify the holder —
//!   the synthesized disposal cascade over such fields frees memory and runs no
//!   user code, so the holder keeps the ceremony exemption (§28.9).
//! cites: 28.9, 28.5
//! expect: quiet
module Conf;

public class Leaf
  public property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;
end Leaf;

public class Holder
  private owned _leaf: Leaf;

  public constructor()
    _leaf := new Leaf(1);
  end constructor;

  public property Id: int => _leaf.Id;
end Holder;

public class Program
  public static method Run()
    var h := new Holder();
    Console.WriteLine(h.Id);
  end Run;
end Program;
