module Probe;
// STATIC REGIME (records): a borrow-bearing record reaches an `Object`-typed parameter (a borrow-erasing position). EXPECT (spec §9.7 borrow-erasing bullet): compile error at the conversion. Candidate A needs the class analogue only at OWNING positions (an owned upcast), see p14.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public record Pair(Item: Item?);
public class Sink
  private _o: Object?;
  public constructor()
  end constructor;
  public method Keep(o: Object?) => _o := o;
end Sink;
public class Program
  public static method Main(): int
    using var s := new Sink();
    using var i := new Item("i");
    s.Keep(new Pair(i));
    return 0;
  end Main;
end Program;
