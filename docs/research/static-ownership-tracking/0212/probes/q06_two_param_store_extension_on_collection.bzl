module Probe;
// OPEN QUESTION (a store that relates two parameters): can the helper of rule case s28_11_0041, `static method Store(a: Array<Item>, it: Item)`, be written today as an extension of the standard collection with a `scoped` parameter? MEASURE on r40. RESULT: no, on two counts — an identifier in an extension head's type-argument list is a type parameter (spec 26.4), so `it` is typed at a type parameter, where `scoped` is not admitted (22.3); and r40 lets a `scoped` parameter be forwarded only to another `scoped` parameter, not to `Add`. Under 0212 the first needs `scoped` at a type parameter (the proposal's 8.7) and the second is a store into the bounding object. Today the rewrite for a standard collection is to store at the caller.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
extension Array<Item>
  public method StoreOne(scoped it: Item): int
    this.Add(it);
    return this.Count;
  end StoreOne;
end;
public class Program
  public static method Main(): int
    using var a := new Item("a");
    using var xs := new Array<Item>();
    Console.WriteLine(xs.StoreOne(scoped a));
    Console.WriteLine(xs[0].Name);
    return 0;
  end Main;
end Program;
