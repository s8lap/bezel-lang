public class Parser
  public record Token(Kind: TokenKind, Text: string, Line: int);

  public enum TokenKind
    Identifier, Number, Operator, EndOfFile;
  end TokenKind;
end Parser;
