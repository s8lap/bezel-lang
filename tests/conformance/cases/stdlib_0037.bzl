public class OwnedArray<T>: IOwnedList<T> where T: class
  public constructor();
  public constructor(capacity: int);

  public readonly property Count: int;
  public readonly property Capacity: int;

  public property [i: int]: T
    get;                                     // borrow of slot
    set(owned);                              // takes owned T; disposes previous value
  end;

  public method Add(owned item: T);
  public method AddRange(owned items: OwnedArray<T>);   // drains items into self
  public method Insert(index: int, owned item: T);
  public method RemoveAt(index: int);                    // disposes the removed element
  public method Remove(item: T): bool;                   // borrow-taking query;
                                                          //   disposes the matched element
  public method Clear();                                  // disposes every element
  public method Contains(item: T): bool;
  public method IndexOf(item: T): int;

  public method Sort(comparer: (T, T) -> int);          // comparer takes borrows
  public method Reverse();

  // Drain methods — transfer ownership of elements out of the collection
  public method PopAt(index: int): owned T;             // removes and returns owned
  public method PopFirst(): owned T;                     // = PopAt(0); raises if empty
  public method PopLast(): owned T;                      // = PopAt(Count - 1); raises if empty
  public method TryPopFirst(out owned item: T): bool;
  public method TryPopLast(out owned item: T): bool;
  public method Drain(): OwnedEnumerable<T>;             // iteration drains the source
end OwnedArray;

public extension OwnedArray<T> where T: class, IComparable<T>
  public method Sort();
end;
