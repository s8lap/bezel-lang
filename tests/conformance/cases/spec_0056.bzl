interface IEnumerable<out T>
  method GetEnumerator(): owned IEnumerator<T>;
end IEnumerable;

interface IComparer<in T>            // illustrative: the standard library declares no
  method Compare(a: T, b: T): int;   //   contravariant interface (`IEqualityComparer<T>`
end IComparer;                       //   is invariant)
