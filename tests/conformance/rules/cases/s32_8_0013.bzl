//! case: s32_8_0013
//! doc: spec
//! section: 32.8
//! title: a lambda that declares the env position as a parameter does not fit the env-stripped callback type
//! rule: The Bezel-visible surface: `sqlite3_exec` above is a **four-argument**
//!   method whose `cb` parameter has type `(int, Ptr<IntPtr>, Ptr<IntPtr>) -> int`;
//!   `qsort_r` is a four-argument method whose `compar` has type
//!   `(IntPtr, IntPtr) -> int`.
//! cites: 32.8, 27.1
//! expect: reject
//! diagnostic: parameter
//! notes: valid twin is s32_8_0019, whose comparator lambda takes two parameters.
module Conf;

extern "libc"
  public method qsort_r(basePtr: IntPtr, nmemb: ulong, size: ulong,
      compar: (IntPtr, IntPtr, env) -> int, env): ();
end;

public static class Sorter
  public static method SortPair(basePtr: IntPtr)
    qsort_r(basePtr, 2UL, 4UL,
        lambda(a: IntPtr, b: IntPtr, user: IntPtr): int => 0);
  end SortPair;
end Sorter;
