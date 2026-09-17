//! case: s32_8_0001
//! doc: spec
//! section: 32.8
//! title: a static method reference is admitted at a plain callback parameter
//! rule: At a plain (un-`env`-paired) function-typed parameter, the callback must be a
//!   static method reference or a non-capturing lambda: both lower to a bare C
//!   function pointer with no environment.
//! cites: 32.8
//! expect: accept
module Conf;

extern "libc"
  method qsort(basePtr: IntPtr, nmemb: UIntPtr, size: UIntPtr,
      compar: (IntPtr, IntPtr) -> int): ();
end;

public static class IntSorter
  public static method CompareInts(a: IntPtr, b: IntPtr): int
    return 0;
  end CompareInts;

  public static method SortInPlace(arrayPtr: IntPtr, count: UIntPtr,
      sizeOfInt: UIntPtr)
    qsort(arrayPtr, count, sizeOfInt, IntSorter.CompareInts);
  end SortInPlace;
end IntSorter;
