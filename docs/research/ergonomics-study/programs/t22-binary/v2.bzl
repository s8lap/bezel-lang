module Files.Binary;

import System.IO;

public class Program
  public static method Main()
    const path := "/var/tmp/bezel-scratch/ergo/t22.bin";
    var values: Array<int> := [1, 256, -2, 1000000];
    do
      using var fs := new FileStream(path, FileMode.Create, FileAccess.Write);
      // r40: Stream.Write(ReadOnlySpan<byte>) is not implemented (#2457); use the Array form.
      var bytes := new Array<byte>(4, 0);
      for var v in values do
        bytes[0] := byte(v & 0xFF);
        bytes[1] := byte((v >> 8) & 0xFF);
        bytes[2] := byte((v >> 16) & 0xFF);
        bytes[3] := byte((v >> 24) & 0xFF);
        fs.Write(bytes, 0, 4);
      end;
    end;
    using var rs := new FileStream(path, FileMode.Open, FileAccess.Read);
    Console.WriteLine($"{rs.Length} bytes");
    var chunk := new Array<byte>(4, 0);
    var sum := 0L;
    var parts := new Array<string>();
    while rs.Read(chunk, 0, 4) = 4 do
      var v := chunk.AsReadOnlySpan().ReadInt32LittleEndian();
      sum += v;
      parts.Add(v.ToString());
    end;
    Console.WriteLine(String.Join(" ", parts));
    Console.WriteLine($"sum {sum}");
    File.Delete(path);
  end Main;
end Program;
