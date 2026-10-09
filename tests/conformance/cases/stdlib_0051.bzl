public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T>, IOwningCollectionLiteralTarget<OwningArray<T>, T> where T: class
  private owned _storage: OwningBuffer<T>;
  private _count: int := 0;

  public constructor()
    this(8);
  end constructor;

  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningArray<T>
    return elements;                                   // the literal's temporary is the result (spec §15.4.1, owning form)
  end FromCollectionLiteral;

  public constructor(capacity: int)
    if capacity < 0 then
      throw new ArgumentOutOfRangeException("capacity");
    end;
    _storage := new OwningBuffer<T>((if capacity > 0 then capacity else 1));
  end constructor;

  public pure property Count: int => _count;   // `pure` is mandatory: §4.1's
                                               //   IReadOnlyCollection<T>.Count is
                                               //   pure-declared, and spec §22.15
                                               //   dispatch rule 1 makes dropping it
                                               //   at an implementation a compile error
  public pure property Capacity: int => _storage.Length;   // `pure` is mandatory: the
                                                //   declared surface above pure-declares
                                                //   Capacity, and the body's only call —
                                                //   OwningBuffer<T>.Length (§4.14) — is
                                                //   pure-declared, so spec §22.15 rule 6
                                                //   admits it

  public property [i: int]: T
    get => do
      BoundsCheck(i);
      return _storage[i]!;
    end;
    set(owned) => do
      BoundsCheck(i);
      _storage[i] := value;                       // owned-setter disposes previous
    end;
  end;

  public method Add(owned item: T)
    if _count = _storage.Length then
      GrowTo(_storage.Length * 2);
    end;
    _storage[_count] := item;
    _count := _count + 1;
  end Add;

  public method PopAt(index: int): owned T
    BoundsCheck(index);
    var taken := move _storage[index];             // taken: T? (owned)
    _storage[index] := null;                        // restore slot
    for var j in index..<(_count - 1) do
      var k := j + 1;                                // the moved index, bound first (spec §28.11)
      var shifted := move _storage[k];               // shifted: T? (owned)
      _storage[k] := null;                            // restore the shifted-from slot, at k
      _storage[j] := shifted;                        // setter on moved-from slot;
                                                      //   no previous to dispose
    end;
    _count := _count - 1;
    return taken!;                                   // i < _count guaranteed non-null
  end PopAt;

  public method RemoveAt(index: int)
    using var _ := PopAt(index);                  // dispose at scope end
  end RemoveAt;

  public method Clear()
    for var i in 0..<_count do
      var item := move _storage[i];                // item: T? (owned)
      _storage[i] := null;                          // restore moved-from slot
      item!.Dispose();                              // slot was non-null
                                                     //   (i < _count guarantees it)
    end;
    _count := 0;
  end Clear;

  // ... and so on for Insert, Remove, Contains, IndexOf, Sort, Reverse,
  //     PopFirst, PopLast, TryPopFirst, TryPopLast, Drain, AddRange.

  private method BoundsCheck(i: int)
    if i < 0 or i >= _count then
      throw new IndexOutOfRangeException();
    end;
  end BoundsCheck;

  private method GrowTo(newCapacity: int)
    var fresh := new OwningBuffer<T>(newCapacity);
    for var i in 0..<_count do
      var moved := move _storage[i];                 // moved: T? (owned)
      _storage[i] := null;                            // restore moved-from slot
      fresh[i] := moved;                              // setter on moved-from slot of fresh
    end;
    _storage.Dispose();                              // dispose old buffer
                                                      //   (now all-null, no slots leak)
    _storage := fresh;
  end GrowTo;

  // No destructor declared; the compiler generates the default Dispose
  // which disposes _storage's contents (every non-null slot's owned T)
  // and the buffer's storage block, in that order.
end OwningArray;
