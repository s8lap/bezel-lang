public interface IReadOnlyCollection<out T>: IEnumerable<T>   pure readonly property Count: int;  end
public interface IReadOnlyList<out T>: IReadOnlyCollection<T>   readonly property [i: int]: T;  end
public interface ICollection<T>: IReadOnlyCollection<T>
  readonly property IsReadOnly: bool;
  method Add(item: T);  Clear();  Contains(item: T): bool;  Remove(item: T): bool;
end
public interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;   method Insert(index: int, item: T);
end
public interface IOwningCollection<T>: IReadOnlyCollection<T>, IDisposable where T: class
  readonly property IsReadOnly: bool;
  method Add(owned item: T);  Clear();               // Clear disposes every element
  method Contains(item: T): bool;                    // borrow-taking query
  method Remove(item: T): bool;                      // disposes the removed element
end
public interface IOwningList<T>: IReadOnlyList<T>, IOwningCollection<T> where T: class
  property [i: int]: T { get; set(owned); }          // the setter disposes the previous value
  method Insert(index: int, owned item: T);
end
