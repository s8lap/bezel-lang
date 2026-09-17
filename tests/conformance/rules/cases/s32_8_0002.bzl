//! case: s32_8_0002
//! doc: spec
//! section: 32.8
//! title: a non-capturing lambda is admitted at a plain callback parameter
//! rule: At a plain (un-`env`-paired) function-typed parameter, the callback must be a
//!   static method reference or a non-capturing lambda.
//! cites: 32.8
//! expect: accept
module Conf;

extern "libc"
  method qsort(basePtr: IntPtr, nmemb: UIntPtr, size: UIntPtr,
      compar: (IntPtr, IntPtr) -> int): ();
end;

public static class IntSorter
  public static method SortInPlace(arrayPtr: IntPtr, count: UIntPtr,
      sizeOfInt: UIntPtr)
    qsort(arrayPtr, count, sizeOfInt, lambda(a: IntPtr, b: IntPtr): int => 0);
  end SortInPlace;
end IntSorter;
