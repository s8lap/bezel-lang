//! case: s28_3_8_9502
//! doc: spec
//! section: 28.3.8
//! title: a bare method call with a record return over a scope-bound local may be returned
//! rule: Value-typed and runtime-managed returns are not borrows and are outside this rule
//! cites: 28.3, 9.7
//! expect: run
//! output: 200
//! notes: Record-typed twin of the s28_3_11_95xx rejects (#9155 fix-forward): the record holds no borrow,
//!   so the bare call over a local is admitted: 200 * 1.
module Conf;

public class Item
  public constructor()
  end constructor;
  destructor
  end destructor;
end Item;

public record R(V: int);

public class C
  public static method Mk(x: Item): R
    return new R(1);
  end Mk;

  public static method F(): R
    var t := new Item();
    return Mk(t);
  end F;

  public static method Main(): int
    var s := 0;
    for var j in 0 ..< 200 do
      s := s + C.F().V;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
