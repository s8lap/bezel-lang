//! case: s32_8_0022
//! doc: spec
//! section: 32.8
//! title: a synchronous callback may make a further extern call
//! rule: A callback that native code invokes on the thread executing an extern call
//!   made by Bezel code runs synchronously inside the calling domain's current
//!   continuation — it is ordinary Bezel code of that continuation: it may touch
//!   domain state under the ordinary rules, make further extern calls …
//! cites: 32.8
//! expect: run
//! stdout: True
module Conf;

extern "libc"
  public method qsort_r(basePtr: IntPtr, nmemb: ulong, size: ulong,
      compar: (IntPtr, IntPtr, env) -> int, env): ();
  public method labs(x: long): long;
end;

public class Program
  public static method Main()
    var bytes: Array<byte> := [2, 0, 0, 0, 1, 0, 0, 0];
    using var raw := RawBytes.From(bytes);
    var total := 0L;
    qsort_r(raw.Pointer, 2UL, 4UL,
        lambda(a: IntPtr, b: IntPtr): int => do
          total := total + labs(-3L);
          return 0;
        end);
    Console.WriteLine((total > 0L) and ((total mod 3L) = 0L));
  end Main;
end Program;
