module Probe;
// OPEN QUESTION (a store that relates two parameters): the helper `Feed(sink, it) => sink.Keep(it)` has no static spelling as a static method. Written as a method of the storing object with a `scoped` parameter, the bounding object is the receiver. MEASURE on r40: is a class-typed `scoped` method parameter admitted with a `using var` receiver declared after the source? (Form 5 of 28.14: "otherwise -> the receiver".)
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
  public method Feed(scoped it: Item)
    Console.WriteLine("feeding");
    Keep(scoped it);
  end Feed;
  public method Last(): string
    var l := _last;
    if l <> null then
      return l.Name;
    end;
    return "none";
  end Last;
end Sink;
public class Program
  public static method Main(): int
    using var a := new Item("a");
    using var sink := new Sink();
    sink.Feed(scoped a);
    Console.WriteLine(sink.Last());
    return 0;
  end Main;
end Program;
