//! case: s32_8_0020
//! doc: spec
//! section: 32.8
//! title: a bound instance-method reference at an env-paired parameter updates this during the call
//! rule: The pair makes the callback parameter **capture-admitting**: any function
//!   value — including a capturing lambda and a bound instance-method reference
//!   `this.OnRow` — may be passed there.
//! cites: 32.8
//! expect: run
//! stdout: True
//! notes: s32_8_0005 is the accept-only twin; this one observes the field write
//!   the callback makes through the recovered receiver.
module Conf;

extern "libc"
  public method qsort_r(basePtr: IntPtr, nmemb: ulong, size: ulong,
      compar: (IntPtr, IntPtr, env) -> int, env): ();
end;

public class PairSorter
  private _calls: int := 0;

  public constructor()
  end constructor;

  private method OnCompare(a: IntPtr, b: IntPtr): int
    _calls := _calls + 1;
    return 0;
  end OnCompare;

  public method Sort(basePtr: IntPtr): bool
    qsort_r(basePtr, 2UL, 4UL, this.OnCompare);
    return _calls > 0;
  end Sort;
end PairSorter;

public class Program
  public static method Main()
    var bytes: Array<byte> := [2, 0, 0, 0, 1, 0, 0, 0];
    using var raw := RawBytes.From(bytes);
    using var sorter := new PairSorter();
    Console.WriteLine(sorter.Sort(raw.Pointer));
  end Main;
end Program;
