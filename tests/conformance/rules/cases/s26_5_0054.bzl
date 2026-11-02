//! case: s26_5_0054
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on an OwningTask receiver is rejected
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules
//! cites: 26.5, 30.3
//! expect: reject
//! diagnostic: Bogus
module Conf;

public class R
  public constructor()
  end constructor;
end R;

public class Program
  public static method Main(): int
    var o: OwningTask<R>? := null;
    if o <> null then
      o.Bogus();
    end;
    return 0;
  end Main;
end Program;
