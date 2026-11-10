module Probe;
// OPEN QUESTION (a store that relates two parameters), the extension form: a helper declared as an extension of the storing type, with a `scoped` parameter forwarded to the type's own `scoped` member. MEASURE on r40: does 28.14 admit `scoped` on an extension method (the receiver parameter as the bounding object)?
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
    _last := it;
  end Keep;
  public method Last(): string
    var l := _last;
    if l <> null then
      return l.Name;
    end;
    return "none";
  end Last;
end Sink;
extension Sink
  public method Feed(scoped it: Item)
    Console.WriteLine("feeding");
    this.Keep(scoped it);
  end Feed;
end;
public class Program
  public static method Main(): int
    using var a := new Item("a");
    using var sink := new Sink();
    sink.Feed(scoped a);
    Console.WriteLine(sink.Last());
    return 0;
  end Main;
end Program;
