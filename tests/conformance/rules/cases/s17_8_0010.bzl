//! case: s17_8_0010
//! doc: spec
//! section: 17.8
//! title: a var bound to a null-forgiving interface property read iterates the owning array
//! rule: A property read returning a borrow of the holder's stored collection, asserted non-null with !, is borrowed with the same source and is iterated without disposing the holder's collection (spec 17.8); 50 iterations sum 3 + 4 + 5 = 12 each (or the V fields 3, 4, 5).
//! cites: 17.8, 28.3
//! expect: run
//! output: total=600
module Conf;
interface ISeq
  readonly property E: IEnumerable<Item>?;
end ISeq;
class Item
  public V: int;
  public constructor(v: int) V := v; end constructor;
end Item;
class Holder : ISeq
  private owned _a: OwningArray<Item>;
  public constructor()
    _a := new OwningArray<Item>(); _a.Add(new Item(3)); _a.Add(new Item(4)); _a.Add(new Item(5));
  end constructor;
  public property E: IEnumerable<Item>?
    get => do
      return _a;
    end;
  end E;
end Holder;
class P
  public static method Main(): int
    var total := 0;
    for var i in 1..50 do
      using var h := new Holder();
      var f: ISeq := h;
      var e := f.E!; for var x in e do total := total + x.V; end;
    end;
    Console.WriteLine("total=" + total.ToString());
    return 0;
  end Main;
end P;
