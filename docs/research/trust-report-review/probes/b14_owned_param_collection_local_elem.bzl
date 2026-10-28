module Conf;
// §28.11 clause 3: a collection an `owned` parameter holds is the body's own; a store of a top-level local's borrow into it is "admitted under the residual". The element is body-sourced, so the 0157 site rule applies: report only at a transfer of `item`; here there is none. EXPECT: quiet (both v0.349 readings agree: no transfer).
public class Item
  public constructor()
  end constructor;
end Item;
public class Program
  public static method Fill(owned list: Array<Item>)
    var item := new Item();
    list.Add(item);
  end Fill;
end Program;
