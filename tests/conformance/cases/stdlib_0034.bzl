public interface IOwnedCollection<T>: IReadOnlyCollection<T>, IDisposable
  where T: class
  readonly property IsReadOnly: bool;
  method Add(owned item: T);
  method Clear();                            // disposes every element
  method Contains(item: T): bool;            // borrow-taking query
  method Remove(item: T): bool;              // disposes the removed element
end IOwnedCollection;

public interface IOwnedList<T>: IReadOnlyList<T>, IOwnedCollection<T>
  where T: class
  property [i: int]: T
    get;
    set(owned);                              // takes owned T; disposes previous value
  end;
  method Insert(index: int, owned item: T);
end IOwnedList;
