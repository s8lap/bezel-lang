module Fs.Tree;

public class Node
  private owned _children: OwningArray<Node> := new OwningArray<Node>();
  private _parent: Node?;
  public readonly property Name: string;

  public constructor(name: string, parent: Node?)
    Name := name;
    _parent := parent;
  end constructor;

  public method Add(name: string): Node
    _children.Add(new Node(name, this));
    return _children[_children.Count - 1];
  end Add;

  public method Path(): string
    var p := _parent;
    if p = null then return "/"; end;
    var up := p.Path();
    return (if up = "/" then $"/{Name}" else $"{up}/{Name}");
  end Path;

  public method Print(depth: int)
    Console.WriteLine($"{String.Repeat("  ", depth)}{Name}");
    for var c in _children do c.Print(depth + 1); end;
  end Print;

  public method Leaves(): int
    if _children.Count = 0 then return 1; end;
    var n := 0;
    for var c in _children do n += c.Leaves(); end;
    return n;
  end Leaves;
end Node;

public class Program
  public static method Main()
    using var root := new Node("/", null);
    var usr := root.Add("usr");
    var bin := usr.Add("bin");
    _ := usr.Add("lib");
    _ := root.Add("etc");
    Console.WriteLine(bin.Path());
    root.Print(0);
    Console.WriteLine($"leaves {root.Leaves()}");
  end Main;
end Program;
