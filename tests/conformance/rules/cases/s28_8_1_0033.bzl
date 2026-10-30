//! case: s28_8_1_0033
//! doc: spec
//! section: 28.8.1
//! title: the replacing move with a with clause on an owned parameter is rejected
//! rule: `move loc with e` — `loc` an owned field `x.f` or an owning slot `coll[i]`, `e` an
//!   expression
//! cites: 22.3, 28.3
//! expect: reject
//! diagnostic: with
module Conf;

public class Res
  public constructor() end constructor;
  destructor end destructor;
end Res;

public class User
  public static method Eat(owned r: Res)
    using var k := move r;
  end Eat;
  public static method Run(owned p: Res)
    Eat(move p with new Res());
  end Run;

  public static method Main(): int
    return 0;
  end Main;
end User;
