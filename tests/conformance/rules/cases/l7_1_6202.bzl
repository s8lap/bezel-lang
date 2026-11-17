//! case: l7_1_6202
//! doc: stdlib
//! section: 7.1
//! title: a zero-length span Read returns 0 although data remains; a
//!   zero-length span Write adds nothing
//! rule: a zero-length window returns `0` immediately, the one carve-out
//! cites: 7.1
//! expect: run
//! stdout: 0 3
module Conf;

import System.IO;

public class Program
  static method Produce(path: string)
    using var src := new OwningBuffer<byte>(3, lambda(i) => byte(i + 1));
    using var w := new FileStream(path, FileMode.Create, FileAccess.Write);
    w.Write(src.AsReadOnlySpan(0, 0));
    w.Write(src.AsReadOnlySpan());
  end Produce;

  static method ReadBack(path: string): string
    using var dst := new OwningBuffer<byte>(8);
    using var r := new FileStream(path, FileMode.Open, FileAccess.Read);
    var z := r.Read(dst.AsSpan(0, 0));
    var total := 0;
    var n := r.Read(dst.AsSpan());
    while n > 0 do
      total := total + n;
      n := r.Read(dst.AsSpan(total));
    end;
    return $"{z} {total}";
  end ReadBack;

  public static method Main()
    var path := "l7_1_6202.tmp";
    Produce(path);
    Console.WriteLine(ReadBack(path));
    File.Delete(path);
  end Main;
end Program;
