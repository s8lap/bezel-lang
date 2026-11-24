//! case: s27_5_7910
//! doc: spec
//! section: 27.5
//! title: an owned class parameter read through captured cells on two branches yields the
//!   field sums
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.3
//! expect: run
//! stdout: 119250
//! notes: Fix pin for a closure-cell leak: the corpus runner has no leak leg, so this asserts
//!   the program output; leak freedom is covered by the gtest rows.
module P;
public class Item
  public n: int;
  public constructor(k: int) n := k; end constructor;
end Item;
public class Program
  public static method Apply(f: () -> int): int => f();
  public static method M(owned it: Item, p: int): int
    var r := 0;
    if p mod 2 = 0 then
      r := Apply(lambda() => it.n + p);
    end if;
    if p mod 3 = 0 then
      do
        r := r + Apply(lambda() => it.n * 2);
      end;
    end if;
    return r + it.n;
  end M;
  public static method Main(): int
    var t := 0;
    for var j in 0..<300 do t := t + Program.M(new Item(j), j); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
