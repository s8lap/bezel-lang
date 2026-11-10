module Tools.Csv;

record Employee(Name: string, Dept: string, Salary: int);

public class Program
  private static method ParseLine(line: string): owned Array<string>
    var fields := new Array<string>();
    var sb := new StringBuilder();
    var inQuotes := false;
    for var c in line do
      if c = '"' then
        inQuotes := not inQuotes;
      elif c = ',' and not inQuotes then
        fields.Add(sb.ToString());
        sb.Clear();
      else
        sb.Append(c);
      end;
    end;
    fields.Add(sb.ToString());
    return fields;
  end ParseLine;

  public static method Main()
    const data := "name,dept,salary\nAnn,Eng,120000\nBob,Ops,80000\n\"Cho, Jr.\",Eng,95000\nDee,Sales,70000\nEve,Ops,85000\n";
    var rows := new Array<Employee>();
    var first := true;
    for var line in data.Split('\n') do
      if line.Length = 0 then continue; end;
      if first then first := false; continue; end;
      using var f := ParseLine(line);
      rows.Add(new Employee(f[0], f[1], int.Parse(f[2])));
    end;
    Console.WriteLine($"rows: {rows.Count}");
    var totals := new Dictionary<string, int>();
    for var g in rows.GroupBy(lambda(e: Employee): string => e.Dept) do
      totals[g.Key] := g.Sum(lambda(e: Employee): int => e.Salary);
    end;
    var depts := totals.Keys.ToArray();
    depts.Sort();
    for var d in depts do
      Console.WriteLine($"{d}: {totals[d]}");
    end;
  end Main;
end Program;
