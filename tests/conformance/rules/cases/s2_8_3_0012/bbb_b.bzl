module B;
import A;

public class W
  public static method Go(): int
    using var n := G<int>.Make();
    return n.Get();
  end Go;
end W;
