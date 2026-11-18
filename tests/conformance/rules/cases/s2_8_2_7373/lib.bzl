module Lib;
public class G
  public static method ShowC<T>(xs: IReadOnlyCollection<T>): string
    for var x in xs do return $"{x}"; end;
    return "";
  end ShowC;
end G;
