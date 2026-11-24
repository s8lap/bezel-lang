module Text.Template;

public static class Template
  public static method Render(tpl: string, vars: Dictionary<string, string>): string
    var sb := new StringBuilder();
    var i := 0;
    while i < tpl.Length do
      if tpl.Substring(i).StartsWith("{{{{") then
        sb.Append("{{");
        i += 4;
      elif tpl.Substring(i).StartsWith("{{") then
        var close := tpl.IndexOf("}}", i + 2);
        if close < 0 then
          sb.Append(tpl.Substring(i));
          break;
        end;
        var expr := tpl.Substring(i + 2, close - i - 2).Trim();
        var bar := expr.IndexOf('|');
        var key := (if bar < 0 then expr else expr.Substring(0, bar).Trim());
        var filter := (if bar < 0 then "" else expr.Substring(bar + 1).Trim());
        var value := vars.GetOrNull(key);
        if value = null then
          sb.Append($"{{{{?{key}}}}}");
        else
          sb.Append((case filter of      // expression case is the parenthesized atom (§19.7)
            "upper" => value.ToUpper(),
            "lower" => value.ToLower(),
            _ => value!));                 // r42: a narrowed local in a case arm is typed string? (B8 family, #6698)
        end;
        i := close + 2;
      else
        sb.Append(tpl[i]);
        i++;
      end;
    end;
    return sb.ToString();
  end Render;
end Template;

public class Program
  public static method Main()
    var vars: Dictionary<string, string> := ["name": "ada", "count": "3"];
    Console.WriteLine(Template.Render("Hello {{name|upper}}, you have {{count}} new {{things}}. {{{{literal}}", vars));
  end Main;
end Program;
