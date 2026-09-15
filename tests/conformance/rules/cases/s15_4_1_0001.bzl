//! case: s15_4_1_0001
//! doc: spec
//! section: 15.4.1
//! title: a user class conforming to ICollectionLiteralTarget is constructed by a sequence literal
//! rule: A class opts into sequence-literal construction by conforming to
//!   `ICollectionLiteralTarget<TSelf, T>`, supplying `FromCollectionLiteral`;
//!   usage is identical syntax to built-in collections (§15.4.1).
//! cites: 15.4.1
//! expect: run
//! stdout: 3
module Conf;

public class Bag<T>: ICollectionLiteralTarget<Bag<T>, T>
  private _items: Array<T>;

  public constructor(capacity: int)
    _items := new Array<T>(capacity);
  end constructor;

  public method Push(item: T) => _items.Add(item);

  public property Count: int => _items.Count;

  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Bag<T>
    var b := new Bag<T>(elements.Length);
    for var i in 0..<elements.Length do
      b.Push(elements[i]);
    end;
    return b;
  end FromCollectionLiteral;
end Bag;

public class Program
  public static method Main()
    using var bag: Bag<int> := [1, 2, 3];
    Console.WriteLine(bag.Count);
  end Main;
end Program;
