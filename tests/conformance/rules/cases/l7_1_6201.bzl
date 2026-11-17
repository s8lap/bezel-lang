//! case: l7_1_6201
//! doc: stdlib
//! section: 7.1
//! title: FileStream.Write(ReadOnlySpan<byte>) writes the whole span and
//!   Read(Span<byte>) reads it back
//! rule: `Write`/`WriteAsync` write the entire window or raise.
//! cites: 7.1, 7.2
//! expect: run
//! stdout: 4 1 2 3 4
module Conf;

import System.IO;

public class Program
  static method Produce(path: string)
    using var src := new OwningBuffer<byte>(4, lambda(i) => byte(i + 1));
    using var w := new FileStream(path, FileMode.Create, FileAccess.Write);
    w.Write(src.AsReadOnlySpan());
  end Produce;

  static method ReadBack(path: string): string
    using var dst := new OwningBuffer<byte>(8);
    using var r := new FileStream(path, FileMode.Open, FileAccess.Read);
    var total := 0;
    var n := r.Read(dst.AsSpan());
    while n > 0 do
      total := total + n;
      n := r.Read(dst.AsSpan(total));
    end;
    return $"{total} {dst[0]} {dst[1]} {dst[2]} {dst[3]}";
  end ReadBack;

  public static method Main()
    var path := "l7_1_6201.tmp";
    Produce(path);
    Console.WriteLine(ReadBack(path));
    File.Delete(path);
  end Main;
end Program;
