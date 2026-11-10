module Parsing.Calc;

public enum TokKind
  Num, Plus, Minus, Star, Slash, LParen, RParen, End;
end TokKind;

public record Token(Kind: TokKind, Value: int, Pos: int);

public class ParseError(message: string): Exception(message)
end ParseError;

public choice Expr
  Num(value: int),
  Neg(operand: Expr),
  Bin(op: char, left: Expr, right: Expr);

  public method Show(): string => (case this of
    Num(var v) => v.ToString(),
    Neg(var e) => $"(- {e.Show()})",
    Bin(var op, var l, var r) => $"({op} {l.Show()} {r.Show()})");

  public method Eval(): int => (case this of
    Num(var v) => v,
    Neg(var e) => -e.Eval(),
    Bin(var op, var l, var r) => (case op of
      '+' => l.Eval() + r.Eval(),
      '-' => l.Eval() - r.Eval(),
      '*' => l.Eval() * r.Eval(),
      _ => (var d := r.Eval(); (if d = 0 then throw new ParseError("division by zero") else l.Eval() / d))));
end Expr;

public class Lexer
  public static method Tokenize(s: string): owned Array<Token>
    var toks := new Array<Token>();
    var i := 0;
    while i < s.Length do
      var c := s[i];
      if c = ' ' then
        i++;
      elif Char.IsDigit(c) then
        var start := i;
        var n := 0;
        while i < s.Length and Char.IsDigit(s[i]) do
          n := n * 10 + (int(s[i]) - int('0'));
          i++;
        end;
        toks.Add(new Token(TokKind.Num, n, start));
      else
        var kind := (case c of
          '+' => TokKind.Plus, '-' => TokKind.Minus, '*' => TokKind.Star, '/' => TokKind.Slash,
          '(' => TokKind.LParen, ')' => TokKind.RParen,
          _ => throw new ParseError($"unexpected '{c}' at {i}"));
        toks.Add(new Token(kind, 0, i));
        i++;
      end;
    end;
    toks.Add(new Token(TokKind.End, 0, s.Length));
    return toks;
  end Tokenize;
end Lexer;

public class Parser(tokens: Array<Token>)
  private _toks: Array<Token> := tokens;
  private _pos: int := 0;

  private method Peek(): Token => _toks[_pos];
  private method Next(): Token
    var t := _toks[_pos];
    _pos++;
    return t;
  end Next;

  public method ParseAll(): Expr
    var e := ParseSum();
    if Peek().Kind <> TokKind.End then
      throw new ParseError($"unexpected token at {Peek().Pos}");
    end;
    return e;
  end ParseAll;

  private method ParseSum(): Expr
    var left := ParseProduct();
    while Peek().Kind = TokKind.Plus or Peek().Kind = TokKind.Minus do
      var op := (if Next().Kind = TokKind.Plus then '+' else '-');
      left := Expr.Bin(op, left, ParseProduct());
    end;
    return left;
  end ParseSum;

  private method ParseProduct(): Expr
    var left := ParseUnary();
    while Peek().Kind = TokKind.Star or Peek().Kind = TokKind.Slash do
      var op := (if Next().Kind = TokKind.Star then '*' else '/');
      left := Expr.Bin(op, left, ParseUnary());
    end;
    return left;
  end ParseProduct;

  private method ParseUnary(): Expr
    if Peek().Kind = TokKind.Minus then
      _ := Next();
      return Expr.Neg(ParseUnary());
    end;
    return ParseAtom();
  end ParseUnary;

  private method ParseAtom(): Expr
    var t := Next();
    case t.Kind of
      TokKind.Num => do return Expr.Num(t.Value); end,
      TokKind.LParen => do
        var e := ParseSum();
        if Next().Kind <> TokKind.RParen then
          throw new ParseError($"expected ) at {t.Pos}");
        end;
        return e;
      end,
      TokKind.End => throw new ParseError($"unexpected end at {t.Pos}"),
      _ => throw new ParseError($"unexpected token at {t.Pos}")
    end;
  end ParseAtom;
end Parser;

public class Program
  public static method Main()
    var inputs: Array<string> := ["1 + 2 * 3", "(1 + 2) * 3", "-4 + 10 / 3", "2 * (3 + ", "8 / (4 - 4)"];
    var first := true;
    for var s in inputs do
      try
        using var toks := Lexer.Tokenize(s);
        using var p := new Parser(toks);
        var e := p.ParseAll();
        if first then
          Console.WriteLine(e.Show());
          first := false;
        end;
        Console.WriteLine($"{s} -> {e.Eval()}");
      catch
        var ex: ParseError => Console.WriteLine($"{s} -> error: {ex.Message}")
      end;
    end;
  end Main;
end Program;
