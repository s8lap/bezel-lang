module L;

public class Box<T>
  public static method Wrap<T2>(): string => U.DefB<T2>();
end Box;

public static class U
  public static method DefB<T>(): string => $"{default(T)}";
end U;
