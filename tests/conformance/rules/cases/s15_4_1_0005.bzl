//! case: s15_4_1_0005
//! doc: spec
//! section: 15.4.1
//! title: the empty literal targeting a user sequence-literal target is admitted
//! rule: The empty literal `[]` targeting a sequence-literal target is
//!   admitted: the borrowing form receives an empty `ReadOnlyArraySpan<U>`,
//!   and the factory is responsible for handling the empty case (§15.4.1).
//! cites: 15.4.1, 15.4
//! expect: run
//! stdout: 0
module Conf;

public class Bag<T>: ICollectionLiteralTarget<Bag<T>, T>
  private owned _items: Array<T>;

  public constructor(capacity: int)
    _items := new Array<T>(capacity);
  end constructor;

  public method Push(item: T)
    _items.Add(item);
  end Push;

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
    using var empty: Bag<int> := [];
    Console.WriteLine(empty.Count);
  end Main;
end Program;
