module Probe;
// Kind 1, a non-immediate ancestor: the root is passed down two constructors; the leaf holds the root. The leaf's store is a field-held admission (one report line); Mid only forwards. Structurally safe while every link is an owned field. EXPECT: compiles; prints Root; one report line.
public class Root
  private _name: string;
  private owned _mid: Mid?;
  public constructor(name: string)
    _name := name;
    _mid := new Mid(this);
  end constructor;
  public property Name: string => _name;
  public method LeafRoot(): string => _mid!.LeafRoot();
end Root;
public class Mid
  private owned _leaf: Leaf;
  public constructor(root: Root)
    _leaf := new Leaf(root);
  end constructor;
  public method LeafRoot(): string => _leaf.RootName();
end Mid;
public class Leaf
  private _root: Root;
  public constructor(root: Root)
    _root := root;
  end constructor;
  public method RootName(): string => _root.Name;
end Leaf;
public class Program
  public static method Main(): int
    using var r := new Root("Root");
    Console.WriteLine(r.LeafRoot());
    return 0;
  end Main;
end Program;
