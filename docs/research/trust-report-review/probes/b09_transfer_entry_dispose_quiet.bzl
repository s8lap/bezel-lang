module Conf;
// s28_3_0166: disposing a collection-held local is a flag-backstopped kill. EXPECT: quiet.
public class Item
  public constructor()
  end constructor;
  public method N(): int => 1;
end Item;
public class Program
  public static method Run()
    var item := new Item();
    var coll := new Array<Item>();
    coll.Add(item);
    item.Dispose();
  end Run;
end Program;
