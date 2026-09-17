//! case: s15_4_1_0006
//! doc: spec
//! section: 15.4.1
//! title: a user owning-form target receives the literal's elements as an owned OwningArray
//! rule: For a class that owns its elements the conformance is
//!   `IOwningCollectionLiteralTarget<C<T>, T>` and the witness is
//!   `FromCollectionLiteral(owned elements: OwningArray<T>): owned C<T>`; the
//!   compiler materializes a temporary owned `OwningArray<U>`, transfers each
//!   element into it in textual order, and transfers the array into the call
//!   (§15.4.1).
//! cites: 15.4.1, 15.4
//! expect: run
//! stdout: 2
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Pool<T>: IOwningCollectionLiteralTarget<Pool<T>, T> where T: class
  private owned _items: OwningArray<T>;

  public constructor(owned items: OwningArray<T>)
    _items := items;
  end constructor;

  public property Count: int => _items.Count;

  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned Pool<T>
    return new Pool<T>(move elements);
  end FromCollectionLiteral;
end Pool;

public class Program
  public static method Main()
    using var pool: Pool<Widget> := [new Widget("a"), new Widget("b")];
    Console.WriteLine(pool.Count);
  end Main;
end Program;
