module Lib;

public class Pair<K, V>
  public class Item
    public method Get(): int
      return 11;
    end Get;
  end Item;

  public static method Make(): owned Item
    return new Item();
  end Make;
end Pair;
