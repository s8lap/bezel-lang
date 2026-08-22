public record QuotRem(Quot: int, Rem: int);

public static pure method DivMod(a: int, b: int): QuotRem
  requires(b <> 0)

  return new QuotRem(a / b, a mod b);
end DivMod;
