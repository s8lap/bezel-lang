//! case: s30_4_6901
//! doc: spec
//! section: 30.4
//! title: a second await of an OwningTask local that the first await dominates is a compile
//!   error
//! rule: A second await through the same local or non-`ref` parameter binding, whatever the
//!   binding's ownership, that the first award dominates on every path is a compile error
//!   at the second await, naming the first
//! cites: 30.4, 30.5, 28.3
//! expect: reject
//! diagnostic: already awaited
//! notes: Single reject site: the second await; deleting it leaves a legal program.
module Conf;
public class Item
  public readonly property V: int;

  public constructor(v: int)
    V := v;
  end constructor;
end Item;
public class C
  public static async method Make(k: int): OwningTask<Item>
    return new Item(k);
  end Make;
  public static async method Main(): Task<int>
    var t := C.Make(1);
    using var a := await t;
    using var b := await t;
    Console.WriteLine(a.V + b.V);
    return 0;
  end Main;
end C;
