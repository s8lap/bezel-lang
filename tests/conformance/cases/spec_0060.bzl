class MyArray<T>
  // ...
end MyArray;

interface IComparable<T>
  method CompareTo(other: T): int;
end IComparable;

record Pair<TFirst, TSecond>(First: TFirst, Second: TSecond);

static class Utils
  public static method Swap<T>(ref a: T, ref b: T)
    var temp := a;
    a := b;
    b := temp;
  end Swap;
end Utils;
