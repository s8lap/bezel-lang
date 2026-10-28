module Conf;
// Family D: a user `scoped` field — the quiescence obligation. EXPECT: report at the declaration.
public class Button
  private scoped _onClick: () -> ();
  public constructor(scoped onClick: () -> ())
    _onClick := onClick;
  end constructor;
  public method Click()
    _onClick();
  end Click;
end Button;
