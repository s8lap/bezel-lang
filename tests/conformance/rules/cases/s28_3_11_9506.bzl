//! case: s28_3_11_9506
//! doc: spec
//! section: 28.3.11
//! title: a bare iterator call over the method's own parameter may be returned as the frame
//! rule: provided every borrow the returned frame carries is sourced from the method's own parameters or
//!   receiver
//! cites: 28.3, 29.4
//! expect: run
//! output: 200
//! notes: Accepting twin of s28_3_11_9501: the same bare iterator call, its argument a plain parameter of
//!   the returning method instead of a scope-bound local. Each of the 200 iterations enumerates one
//!   element: 200.
module Conf;

public class Item
  public constructor()
  end constructor;
  destructor
  end destructor;
end Item;

public class C
  public static method OnceItem(x: Item): IEnumerable<Item>
    yield return x;
  end OnceItem;

  public static method F(t: Item): IEnumerable<Item>
    return OnceItem(t);
  end F;

  public static method Main(): int
    var n := 0;
    for var j in 0 ..< 200 do
      using var t := new Item();
      for var x in C.F(t) do
        n := n + 1;
      end for;
    end for;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
