//! case: s28_3_8_9501
//! doc: spec
//! section: 28.3.8
//! title: a bare method call with an int return over a scope-bound local may be returned
//! rule: Value-typed and runtime-managed returns are not borrows and are outside this rule
//! cites: 28.3
//! expect: run
//! output: 600
//! notes: Value-typed twin of the s28_3_11_95xx rejects (#9155 fix-forward): the bare call's result is an
//!   int, carries no borrow of the local, and stays admitted: 200 * 3.
module Conf;

public class Item
  public constructor()
  end constructor;
  destructor
  end destructor;
end Item;

public class C
  public static method Num(x: Item): int
    return 3;
  end Num;

  public static method F(): int
    var t := new Item();
    return Num(t);
  end F;

  public static method Main(): int
    var s := 0;
    for var j in 0 ..< 200 do
      s := s + C.F();
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
