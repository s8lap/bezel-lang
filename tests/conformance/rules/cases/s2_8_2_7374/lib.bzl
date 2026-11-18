module Lib;
public class G
  public static method ShowE<T>(xs: IEnumerable<T>): string
    for var x in xs do return $"{x}"; end;
    return "";
  end ShowE;
end G;
