module Parsing.Calc;

public enum TokKind
  Num, Plus, Minus, Star, Slash, LParen, RParen, End;
end TokKind;

public record Token(Kind: TokKind, Value: int, Pos: int);

public class ParseError(message: string): Exception(message)
end ParseError;

// A choice cannot contain itself (§35.1), so the AST is a class hierarchy with owned children.
public abstract class Expr
  public abstract method Show(): string;
  public abstract method Eval(): int;
end Expr;

public sealed class Num(value: int): Expr
  private _v: int := value;
  public override method Show(): string => _v.ToString();
  public override method Eval(): int => _v;
end Num;

public sealed class Neg: Expr
  private owned _e: Expr;
  public constructor(owned e: Expr)
    _e := e;
  end constructor;
  public override method Show(): string => $"(- {_e.Show()})";
  public override method Eval(): int => -_e.Eval();
end Neg;

public sealed class Bin: Expr
  private _op: char;
  private owned _l: Expr;
  private owned _r: Expr;
  public constructor(op: char, owned l: Expr, owned r: Expr)
    _op := op;
    _l := l;
    _r := r;
  end constructor;
  public override method Show(): string => $"({_op} {_l.Show()} {_r.Show()})";
  public override method Eval(): int
    var a := _l.Eval();
    var b := _r.Eval();
    return (case _op of
      '+' => a + b,
      '-' => a - b,
      '*' => a * b,
      _ => (if b = 0 then throw new ParseError("division by zero") else a / b));
  end Eval;
end Bin;

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

  public method ParseAll(): owned Expr
    var e := ParseSum();
    if Peek().Kind <> TokKind.End then
      throw new ParseError($"unexpected token at {Peek().Pos}");
    end;
    return e;
  end ParseAll;

  private method ParseSum(): owned Expr
    var left := ParseProduct();
    while Peek().Kind = TokKind.Plus or Peek().Kind = TokKind.Minus do
      var op := (if Next().Kind = TokKind.Plus then '+' else '-');
      left := new Bin(op, move left, ParseProduct());
    end;
    return left;
  end ParseSum;

  private method ParseProduct(): owned Expr
    var left := ParseUnary();
    while Peek().Kind = TokKind.Star or Peek().Kind = TokKind.Slash do
      var op := (if Next().Kind = TokKind.Star then '*' else '/');
      left := new Bin(op, move left, ParseUnary());
    end;
    return left;
  end ParseProduct;

  private method ParseUnary(): owned Expr
    if Peek().Kind = TokKind.Minus then
      _ := Next();
      return new Neg(ParseUnary());
    end;
    return ParseAtom();
  end ParseUnary;

  private method ParseAtom(): owned Expr
    var t := Next();
    case t.Kind of
      TokKind.Num => do return new Num(t.Value); end,
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
        using var e := p.ParseAll();
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
