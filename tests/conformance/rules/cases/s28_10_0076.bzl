//! case: s28_10_0076
//! doc: spec
//! section: 28.10
//! title: a using var over b! pins the borrow's source so a move of the source in the scope is
//!   rejected
//! rule: the frozen kills (a move, a transfer, a reassignment of the binding) are compile errors
//! cites: 17.8, 28.3
//! expect: reject
//! diagnostic: borrow
module Conf;

public sealed class R
  public constructor() end constructor;
  destructor end destructor;
  public method Use(): int => 1;
  public static method Take(owned r: R)
    using var k := move r;
  end Take;
end R;

public class Program
  public static method Body()
    var a: R? := new R();
    var b := a;
    using var p := b!;
    R.Take(move a!);
  end Body;

  public static method Main(): int
    Body();
    return 0;
  end Main;
end Program;
