//! case: s28_3_0326
//! doc: spec
//! section: 28.3
//! title: a loop that moves an owning-choice local through ! with no reassignment is rejected
//! rule: owned ⊓ moved-from and owned ⊓ explicitly-disposed = **maybe-consumed** — unreadable after
//!   the join, no flow around a back edge into a use; scope-end disposal per-path;
//! cites: K7, 15.3, 17.8
//! expect: reject
//! diagnostic: moved-from
module Conf;

public class Res
  public constructor() end constructor;
  destructor end destructor;
end Res;

public choice OC
  Text(owned r: Res),
  Empty;
end OC;

public class Program
  public static method T()
    var o: OC? := OC.Text(new Res());
    for var j in 0..<2 do
      var x := o!;
    end;
  end T;

  public static method Main(): int
    T();
    return 0;
  end Main;
end Program;
