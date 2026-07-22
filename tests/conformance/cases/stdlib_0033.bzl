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
  // (§28.11 v0.213, #2081: `DisposeElements()` removed from ICollection<T> with
  // axiom A8 — a borrowing collection never disposes its elements.)
end ICollection;

public interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
  method Insert(index: int, item: T);
end IList;
