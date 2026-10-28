module Conf;
// Candidate: `holder.Hold(new Item())` — an unbound temporary passed to a plain parameter the callee stores (§28.3 clause 3: "admitted at the call ... a temporary meets the obligation only when the object dies within the same full expression"). Here `holder` outlives the statement and is later used: a certain dangle in the CALLER. Measure r35 (verdict, report, run).
public class Item
  private _n: int := 3;
  public constructor()
  end constructor;
  public method N(): int => _n;
end Item;
public class Holder
  private _item: Item?;
  public constructor()
  end constructor;
  public method Hold(item: Item)
    _item := item;
  end Hold;
  public method Peek(): int => _item!.N();
end Holder;
public class Program
  public static method Main(): int
    using var holder := new Holder();
    holder.Hold(new Item());
    return holder.Peek();
  end Main;
end Program;
