//! case: s28_8_1_0032
//! doc: spec
//! section: 28.8.1
//! title: the replacing move with a with clause on a local binding is rejected at a local binding
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
  public static method Run()
    var a := new Res();
    using var b := move a with new Res();
  end Run;

  public static method Main(): int
    return 0;
  end Main;
end User;
