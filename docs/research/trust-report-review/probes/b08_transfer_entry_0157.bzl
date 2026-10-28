module Conf;
// Proposal 0157 (v0.333) transfer entry (s28_3_0162): a local held by a local collection, then transferred. EXPECT: report at `Take(move item)`.
public class Item
  public constructor()
  end constructor;
  public method N(): int => 1;
end Item;
public class Program
  public static method Take(owned i: Item)
  end Take;
  public static method Run()
    var item := new Item();
    var coll := new Array<Item>();
    coll.Add(item);
    Take(move item);
  end Run;
end Program;
