//! case: l7_1_6206
//! doc: stdlib
//! section: 7.1
//! title: span-typed parameters carry the window through to FileStream.Read
//!   and Write
//! rule: public method Read(buffer: Span<byte>): int;
//! cites: 7.1
//! expect: run
//! stdout: 4 2 2 0
module Conf;

import System.IO;

public class Program
  static method Put(f: FileStream, s: ReadOnlySpan<byte>)
    f.Write(s);
  end Put;

  static method Fill(f: FileStream, s: Span<byte>): int
    var total := 0;
    var n := f.Read(s);
    while n > 0 do
      total := total + n;
      n := f.Read(s.Slice(total));
    end;
    return total;
  end Fill;

  static method Produce(path: string)
    using var src := new OwningBuffer<byte>(4, lambda(i) => byte(2));
    using var w := new FileStream(path, FileMode.Create, FileAccess.Write);
    Put(w, src.AsReadOnlySpan());
  end Produce;

  static method ReadBack(path: string): string
    using var dst := new OwningBuffer<byte>(6);
    using var r := new FileStream(path, FileMode.Open, FileAccess.Read);
    var n := Fill(r, dst.AsSpan());
    return $"{n} {dst[0]} {dst[3]} {dst[4]}";
  end ReadBack;

  public static method Main()
    var path := "l7_1_6206.tmp";
    Produce(path);
    Console.WriteLine(ReadBack(path));
    File.Delete(path);
  end Main;
end Program;
