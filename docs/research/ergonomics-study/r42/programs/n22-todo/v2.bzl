module Tools.Todo;

import System.IO;

public record Item(Done: bool, Text: string);

public static class Store
  private const Dir: string := "/var/tmp/bezel-scratch/ergo2/run/todo";

  public static property PathName: string => Path.Combine(Dir, "todo.txt");

  public static method Load(): owned Array<Item>
    var items := new Array<Item>();
    if not File.Exists(PathName) then return items; end;
    for var line in File.ReadAllLines(PathName) do
      if line.Length < 2 then continue; end;
      items.Add(new Item(line[0] = 'x', line.Substring(2)));
    end;
    return items;
  end Load;

  public static method Save(items: Array<Item>)
    Directory.CreateDirectory(Dir);
    var lines := new Array<string>();
    for var it in items do
      lines.Add($"{(if it.Done then "x" else "-")} {it.Text}");
    end;
    File.WriteAllLines(PathName, lines);
  end Save;
end Store;

public class Program
  private static method Usage(): int
    Console.Error.WriteLine("usage: todo add <text...> | list | done <n> | rm <n>");
    return 2;
  end Usage;

  private static method Index(arg: string, count: int): int?
    var n := int.ParseOrNull(arg);
    if n = null or n < 1 or n > count then
      Console.Error.WriteLine($"todo: no item {arg}");
      return null;
    end;
    return n - 1;
  end Index;

  public static method Main(args: Array<string>): int
    if args.Count = 0 then return Usage(); end;
    using var items := Store.Load();
    case args[0] of
      "add" => do
        if args.Count < 2 then return Usage(); end;
        items.Add(new Item(false, String.Join(" ", args[1..])));
      end,
      "list" => do
        for var i in 0..<items.Count do
          Console.WriteLine($"{i + 1}. [{(if items[i].Done then "x" else " ")}] {items[i].Text}");
        end;
        return 0;
      end,
      "done" => do
        var i := (if args.Count < 2 then null else Index(args[1], items.Count));
        if i = null then return 1; end;
        items[i] := items[i] with(Done := true);
      end,
      "rm" => do
        var i := (if args.Count < 2 then null else Index(args[1], items.Count));
        if i = null then return 1; end;
        items.RemoveAt(i);
      end,
      _ => do return Usage(); end   // `=> return x` needs a do block (as F086)
    end;
    Store.Save(items);
    return 0;
  end Main;
end Program;
