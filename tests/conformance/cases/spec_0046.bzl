public interface IReadOnlyCollection<out T>: IEnumerable<T>
  readonly property Count: int;
end IReadOnlyCollection;

public interface IReadOnlyList<out T>: IReadOnlyCollection<T>
  readonly property [i: int]: T;
end IReadOnlyList;

public interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
end IList;
