//! case: s26_5_0054
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on an OwningTask receiver is rejected
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type.
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
