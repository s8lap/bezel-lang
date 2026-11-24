module Text.Wrap;

public static class Wrapper
  public static method Wrap(text: string, width: int): owned Array<string>
    var lines := new Array<string>();
    var cur := new StringBuilder();
    for var w in text.Split(' ').Where(lambda(s) => s.Length > 0) do
      if cur.Length > 0 and cur.Length + 1 + w.Length > width then
        lines.Add(cur.ToString());
        _ := cur.Clear();
      end;
      if cur.Length > 0 then _ := cur.Append(' '); end;
      _ := cur.Append(w);
    end;
    if cur.Length > 0 then lines.Add(cur.ToString()); end;
    return lines;
  end Wrap;
end Wrapper;

public class Program
  public static method Main()
    const width := 28;
    Console.WriteLine("[ Wrap demo ]".PadCenter(width, '='));
    using var lines := Wrapper.Wrap("The quick brown fox jumps over the lazy dog while supercalifragilistic words test the edge cases.", width);
    for var l in lines do
      Console.WriteLine($"|{l.PadRight(width)}|");
    end;
  end Main;
end Program;
