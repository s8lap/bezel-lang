// RUN: %bezelc -c -o %t.o %s
//
// Issue #2198 / spec §21.10 (v0.310): `record` and `enum` are permitted in
// statement position (§21.10 grammar A.6 LocalTypeDecl). Before the fix,
// `parseStatementImpl` had no case for these keywords and fell through to
// `parseExpression`, which produced "expected expression" for anything that
// wasn't a valid expression.
//
// The fix implements LocalTypeDecl in four places:
//   1. Parser (§21.10): `parseStatementImpl` recognizes `record`/`enum` and
//      wraps the parsed TypeDecl in a LocalTypeDecl statement.
//   2. SymbolCollection (slice 1): `collectTypeDecl` was moved out of the
//      anonymous namespace so TypeResolution and ExprTC can call it.
//   3. TypeResolution (slice 2): `resolveTypesInStmt` creates the TypeSymbol
//      and adds it to ctx_.module()->children; also resolves member types.
//   4. ExprTC (slice 3): `visitStmt` registers the TypeSymbol in scope.
//
// This test verifies that local record and enum declarations compile cleanly
// and that the declared type is usable in subsequent statements.
module M2198;

class C
  public static method Main(): int
    // §21.10 LocalTypeDecl: local record used after declaration.
    record Point
      public X: int;
      public Y: int;
    end Point;

    var p := new Point(1, 2);
    var x := p.X + p.Y;

    // §21.10 LocalTypeDecl: local enum used after declaration.
    enum Color
      Red, Green, Blue;
    end Color;

    var c := Color.Green;
    var idx := int(c);

    // Local type referenced in a subsequent local type declaration.
    record Box
      public Value: int;
      public Inner: Point;
    end Box;

    return x + idx;
  end Main;
end C;
