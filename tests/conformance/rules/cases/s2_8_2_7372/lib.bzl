module Lib;
public class G
  public static method ShowL<T>(xs: IList<T>): string
    for var x in xs do return $"{x}"; end;
    return "";
  end ShowL;
end G;
