module Dep;
public interface IMaker
  static method Make(): int;
end IMaker;
public class K: IMaker
  public constructor()
  end constructor;
  public static method Make(): int => 42;
end K;
public class B
  public constructor()
  end constructor;
  public virtual method Z<T>(a: T): int where T: IMaker => T.Make();
end B;
