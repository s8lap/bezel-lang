module Conf;
// Family B: Insert / Push / Enqueue / AddFirst / Dictionary.Add value slot, each with a parameter borrow into a field-held borrowing collection. EXPECT: five reports.
public class Item
  public constructor()
  end constructor;
end Item;
public class Index
  private owned _a: Array<Item>;
  private owned _s: Stack<Item>;
  private owned _q: Queue<Item>;
  private owned _l: LinkedList<Item>;
  private owned _d: Dictionary<int, Item>;
  public constructor()
    _a := new Array<Item>();
    _s := new Stack<Item>();
    _q := new Queue<Item>();
    _l := new LinkedList<Item>();
    _d := new Dictionary<int, Item>();
  end constructor;
  public method Register(item: Item)
    _a.Insert(0, item);
    _s.Push(item);
    _q.Enqueue(item);
    _l.AddFirst(item);
    _d.Add(1, item);
  end Register;
end Index;
