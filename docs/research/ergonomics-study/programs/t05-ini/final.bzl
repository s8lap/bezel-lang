module Tools.Ini;

public class IniFile
  private owned _sections: OwningDictionary<string, Dictionary<string, string>> := new OwningDictionary<string, Dictionary<string, string>>();

  public static method Parse(text: string): owned IniFile
    var ini := new IniFile();
    var current := "";
    var lineNo := 0;
    for var raw in text.Split('\n') do
      lineNo++;
      var line := raw.Trim();
      if line.Length = 0 or line.StartsWith(";") or line.StartsWith("#") then
        continue;
      end;
      if line.StartsWith("[") and line.EndsWith("]") then
        current := line.Substring(1, line.Length - 2).Trim();
        ini._sections[current] := new Dictionary<string, string>();
        continue;
      end;
      var eq := line.IndexOf('=');
      if eq < 0 then
        Console.WriteLine($"line {lineNo}: syntax error");
        continue;
      end;
      var key := line.Substring(0, eq).Trim();
      var value := line.Substring(eq + 1).Trim();
      ini._sections[current][key] := value;
    end;
    return ini;
  end Parse;

  public method Get(section: string, key: string): string?
    var sec := _sections.GetOrNull(section);
    return sec?.GetOrNull(key);
  end Get;

  public method GetInt(section: string, key: string, fallback: int): int
    var s := Get(section, key);
    if s = null then return fallback; end;
    var n: int;
    if int.TryParse(s, out n) then return n; end;
    return fallback;
  end GetInt;

  public method Dump()
    for var name in _sections.Keys do
      Console.WriteLine($"[{name}]");
      for var kv in _sections[name] do
        Console.WriteLine($"  {kv.Key}={kv.Value}");
      end;
    end;
  end Dump;
end IniFile;

public class Program
  public static method Main()
    const text := "; sample\n[server]\nhost = example.com\nport = 8080\n\n# client part\n[client]\nretries=3\nthis is junk\nname = bezel \n";
    using var ini := IniFile.Parse(text);
    ini.Dump();
    var port := ini.GetInt("server", "port", 80);
    var timeout := ini.GetInt("server", "timeout", 30);
    Console.WriteLine($"port={port} timeout={timeout}");
  end Main;
end Program;
