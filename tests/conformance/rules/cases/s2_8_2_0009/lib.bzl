module Lib;

public class Tree<T>
  public class Node
    public class Leaf
      public method Get(): int
        return 7;
      end Get;
    end Leaf;
  end Node;

  public static method Make(): owned Node.Leaf
    return new Node.Leaf();
  end Make;
end Tree;
