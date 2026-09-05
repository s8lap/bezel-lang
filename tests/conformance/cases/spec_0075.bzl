public class RingBuffer<T>: ICollectionLiteralTarget<RingBuffer<T>, T>
  private owned _slots: Array<T>;
  private _head: int;

  public constructor(capacity: int)
    _slots := new Array<T>(capacity);
    _head := 0;
  end;

  public method Push(item: T) ... end;

  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned RingBuffer<T>
    var rb := new RingBuffer<T>(elements.Length);
    for var i in 0..<elements.Length do
      rb.Push(elements[i]);
    end;
    return rb;
  end FromCollectionLiteral;
end RingBuffer;

// Usage — identical syntax to built-in collections:
using var rb: RingBuffer<int> := [1, 2, 3, 4];
using var empty: RingBuffer<int> := [];
