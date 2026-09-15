//! case: s32_8_0004
//! doc: spec
//! section: 32.8
//! title: a capturing lambda is rejected at a plain callback parameter
//! rule: A bound instance-method reference (`obj.Method`) and a capturing lambda are
//!   rejected there -- each carries an environment (the receiver / the captures)
//!   that a bare function pointer cannot represent.
//! cites: 32.8
//! expect: reject
//! diagnostic: environment
module Conf;

extern "libc"
  method qsort(basePtr: IntPtr, nmemb: UIntPtr, size: UIntPtr,
      compar: (IntPtr, IntPtr) -> int): ();
end;

public static class IntSorter
  public static method SortWithBias(arrayPtr: IntPtr, count: UIntPtr,
      sizeOfInt: UIntPtr, bias: int)
    qsort(arrayPtr, count, sizeOfInt, lambda(a: IntPtr, b: IntPtr): int => bias);
  end SortWithBias;
end IntSorter;
