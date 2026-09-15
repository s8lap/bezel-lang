//! case: s32_8_0003
//! doc: spec
//! section: 32.8
//! title: a bound instance-method reference is rejected at a plain callback parameter
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

public class Sorter
  public constructor()
  end constructor;

  public method CompareInts(a: IntPtr, b: IntPtr): int
    return 0;
  end CompareInts;

  public method SortInPlace(arrayPtr: IntPtr, count: UIntPtr,
      sizeOfInt: UIntPtr)
    qsort(arrayPtr, count, sizeOfInt, this.CompareInts);
  end SortInPlace;
end Sorter;
