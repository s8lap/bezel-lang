//! case: s32_8_0019
//! doc: spec
//! section: 32.8
//! title: a capturing lambda at an env-paired parameter runs during the call and its captures see the writes
//! rule: **Execution context**: a sanctioned invocation is the same-thread case
//!   above — ordinary code of the calling continuation.
//! cites: 32.8, 27.5
//! expect: run
//! stdout: True
//! notes: qsort_r of two 4-byte elements must compare at least once; a comparator
//!   that never ran, or ran against a copy of the captured cell, prints False.
module Conf;

extern "libc"
  public method qsort_r(basePtr: IntPtr, nmemb: ulong, size: ulong,
      compar: (IntPtr, IntPtr, env) -> int, env): ();
end;

public class Program
  public static method Main()
    var bytes: Array<byte> := [2, 0, 0, 0, 1, 0, 0, 0];
    using var raw := RawBytes.From(bytes);
    var calls := 0;
    qsort_r(raw.Pointer, 2UL, 4UL,
        lambda(a: IntPtr, b: IntPtr): int => do
          calls := calls + 1;
          return 0;
        end);
    Console.WriteLine(calls > 0);
  end Main;
end Program;
