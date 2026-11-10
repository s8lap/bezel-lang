module Probe;
// FINDING: a member access through a class-typed `scoped` parameter inside the callee. Spec 28.14 (scoped construction, callee side) bars returning it and storing it elsewhere; it is a passive borrow and "passing it as a plain argument is an ordinary borrow argument". EXPECT (spec): compiles; prints. MEASURE r40.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public class Sink
  private _last: Item? := null;
  public constructor()
  end constructor;
  public method Keep(scoped it: Item)
    Console.WriteLine("keeping " + it.Name);
    _last := it;
  end Keep;
end Sink;
public class Program
  public static method Main(): int
    using var a := new Item("a");
    using var sink := new Sink();
    sink.Keep(scoped a);
    return 0;
  end Main;
end Program;
