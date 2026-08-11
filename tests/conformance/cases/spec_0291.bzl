extern "libc"
  public method qsort(
    base: IntPtr,
    nmemb: UIntPtr,
    size: UIntPtr,
    compar: (IntPtr, IntPtr) -> int): ();
end;

public static class IntSorter
  public static method CompareInts(a: IntPtr, b: IntPtr): int
    // ...
  end CompareInts;

  public static method SortInPlace(arrayPtr: IntPtr, count: UIntPtr, sizeOfInt: UIntPtr)
    qsort(arrayPtr, count, sizeOfInt, IntSorter.CompareInts);
  end SortInPlace;
end IntSorter;
