//! case: s28_11_4804
//! doc: spec
//! section: 28.11
//! title: a name read through an OwningArray class element borrows the collection's slot and sums to 290
//! rule: borrow out (source: the collection)
//! cites: 28.9
//! expect: run
//! output: 290
module Conf;

public class Item
  public Name: string;

  public constructor(n: string)
    Name := n;
  end constructor;

  destructor
  end destructor;
end Item;

public class P
  public static method Get(n: int): int
    var a := new OwningArray<Item>();
    a.Add(new Item($"i{n}"));
    return a[0].Name.Length;
  end Get;
end P;

public class Program
  public static method Main()
    var i := 0;
    var t := 0;
    while i < 100 do
      t := t + P.Get(i);
      i := i + 1;
    end while;
    Console.WriteLine(t);
  end Main;
end Program;
