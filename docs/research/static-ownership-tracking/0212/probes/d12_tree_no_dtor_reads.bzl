module Probe;
// DESTRUCTOR WINDOW, control: the same tree, with a destructor that does NOT read the parent. EXPECT: clean in both modes. (A rule keyed on "the child is not trivial-dispose" would reject this tree: OwningArray is never trivial-dispose.)
public class Node
  private _name: string;
  private _parent: Node?;
  private owned _kids: OwningArray<Node> := new OwningArray<Node>();
  public constructor(name: string, parent: Node?)
    _name := name;
    _parent := parent;
  end constructor;
  public property Name: string => _name;
  public method Path(): string
    var p := _parent;
    if p <> null then
      return p.Path() + "/" + _name;
    end;
    return _name;
  end Path;
  public method AddKid(name: string)
    _kids.Add(new Node(name, this));
  end AddKid;
  public method First(): Node => _kids[0];
  destructor
    Console.WriteLine($"{_name} disposed");
  end destructor;
end Node;
public class Program
  public static method Main(): int
    using var root := new Node("root", null);
    root.AddKid("a");
    root.First().AddKid("b");
    Console.WriteLine(root.First().First().Path());
    return 0;
  end Main;
end Program;
