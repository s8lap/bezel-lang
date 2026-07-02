public interface IOwningCollection<T>: IReadOnlyCollection<T>, IDisposable
  where T: class
  readonly property IsReadOnly: bool;
  method Add(owned item: T);
  method Clear();                            // disposes every element
  method Contains(item: T): bool;            // borrow-taking query
  method Remove(item: T): bool;              // disposes the removed element
end IOwningCollection;

public interface IOwningList<T>: IReadOnlyList<T>, IOwningCollection<T>
  where T: class
  property [i: int]: T
    get;
    set(owned);                              // takes owned T; disposes previous value
  end;
  method Insert(index: int, owned item: T);
end IOwningList;
