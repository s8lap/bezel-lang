module Probe;
// FROM THE ADVERSARIAL CRITIQUE of the first draft (f72926c68), probe c03: a back-pointing child taken out of its owner by a replacing move OUTSIDE a member. 0212 §8.2 source-set clause 3 gives it "the instance's set" (empty for the owner), not the owner's binding. EXPECT today: compiles; checked = DisposedAccessException; unchecked = undefined. Under 0212 revision 2: a value taken out by a replacing `move` carries the binding that holds the instance it left, so `k := move n.Kid with null` is the K2.3 error (finding 4). r40 does not implement the replacing `move`, so this one is argued.
public class Node
  public owned Kid: Child?;
  private _name: string;
  public constructor(name: string)
    _name := name;
    Kid := new Child(this);
  end constructor;
  public property Name: string => _name;
end Node;
public class Child
  private _parent: Node;
  public constructor(p: Node)
    _parent := p;
  end constructor;
  public method ParentName(): string => _parent.Name;
end Child;
public class Program
  public static method Main(): int
    var k: Child? := null;
    do
      using var n := new Node("ann");
      k := move n.Kid with null;
    end;
    Console.WriteLine(k!.ParentName());
    return 0;
  end Main;
end Program;
