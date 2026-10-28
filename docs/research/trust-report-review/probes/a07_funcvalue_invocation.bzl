module Conf;
// Family A: `_v := make()` with `make: () -> Item` — admitted under clause 3's residual (spec §28.3 clause 2 text names it). EXPECT: report.
public class Item
  public constructor()
  end constructor;
end Item;
public class Holder
  private _v: Item;
  public constructor(make: () -> Item)
    _v := make();
  end constructor;
end Holder;
