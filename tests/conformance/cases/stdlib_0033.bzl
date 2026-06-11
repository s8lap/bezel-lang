public interface IReadOnlyCollection<out T>: IEnumerable<T>
  readonly property Count: int;
end IReadOnlyCollection;

public interface IReadOnlyList<out T>: IReadOnlyCollection<T>
  readonly property [i: int]: T;
end IReadOnlyList;

public interface ICollection<T>: IReadOnlyCollection<T>
  readonly property IsReadOnly: bool;
  method Add(item: T);
  method Clear();
  method Contains(item: T): bool;
  method Remove(item: T): bool;
  method DisposeElements();
end ICollection;

public interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
  method Insert(index: int, item: T);
end IList;
