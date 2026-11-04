//! case: s32_8_0021
//! doc: spec
//! section: 32.8
//! title: a static method callback at a plain parameter is invoked and its result reaches native code
//! rule: At a **plain** (un-`env`-paired) function-typed parameter, the callback must
//!   be a static method reference or a non-capturing lambda: both lower to a bare C
//!   function pointer with no environment.
//! cites: 32.8
//! expect: run
//! output: True
//!   False
//! notes: bsearch over one element compares the key with it once; a comparator
//!   answering 0 finds it (non-null), one answering 1 does not (NULL).
module Conf;

extern "libc"
  public method bsearch(key: IntPtr, basePtr: IntPtr, nmemb: ulong, size: ulong,
      compar: (IntPtr, IntPtr) -> int): IntPtr;
end;

public static class Cmp
  public static method Same(a: IntPtr, b: IntPtr): int
    return 0;
  end Same;

  public static method After(a: IntPtr, b: IntPtr): int
    return 1;
  end After;
end Cmp;

public class Program
  public static method Main()
    var bytes: Array<byte> := [7, 0, 0, 0];
    using var key := RawBytes.From(bytes);
    using var table := RawBytes.From(bytes);
    var hit := bsearch(key.Pointer, table.Pointer, 1UL, 4UL, Cmp.Same);
    var miss := bsearch(key.Pointer, table.Pointer, 1UL, 4UL, Cmp.After);
    Console.WriteLine(hit <> IntPtr.Zero);
    Console.WriteLine(miss <> IntPtr.Zero);
  end Main;
end Program;
