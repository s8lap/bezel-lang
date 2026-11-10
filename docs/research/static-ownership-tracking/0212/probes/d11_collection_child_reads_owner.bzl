module Probe;
// DESTRUCTOR WINDOW, an owning collection of back-pointing children (the tree with parent pointers): each node's destructor reads its parent during the parent's cascade. MEASURE: checked = fault at the first child; unchecked = ?
public class Node
  private _name: string;
  private _parent: Node?;
  private owned _kids: OwningArray<Node> := new OwningArray<Node>();
  public constructor(name: string, parent: Node?)
    _name := name;
    _parent := parent;
  end constructor;
  public property Name: string => _name;
  public method AddKid(name: string)
    _kids.Add(new Node(name, this));
  end AddKid;
  destructor
    var p := _parent;
    if p <> null then
      Console.WriteLine($"{_name} leaves {p.Name}");
    else
      Console.WriteLine($"{_name} is the root");
    end;
  end destructor;
end Node;
public class Program
  public static method Main(): int
    using var root := new Node("root", null);
    root.AddKid("a");
    root.AddKid("b");
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
