interface IEnumerable<out T>
  method GetEnumerator(): owned IEnumerator<T>;
end IEnumerable;

interface IComparer<in T>
  method Compare(a: T, b: T): int;
end IComparer;
