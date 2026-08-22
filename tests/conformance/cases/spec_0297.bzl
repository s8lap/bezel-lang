import System.Interop;
import System.IO;                    // IOException (§31.4; stdlib §7.5)

public class RowCollector
  private _rows: int := 0;
  private _lastCell: string := "";

  private method OnRow(argc: int, argv: Ptr<IntPtr>, cols: Ptr<IntPtr>): int
    _rows := _rows + 1;                           // updating `this` — same thread, same continuation
    var i := 0;
    while i < argc do
      var cell := argv.LoadAt(i);                 // char* — NULL for SQL NULL
      if cell <> IntPtr.Zero then                 // guard required: FromCStringPointer raises on Zero (stdlib §10.4)
        _lastCell := string.FromCStringPointer(cell);   // copies; no ownership taken
      end;
      i := i + 1;
    end;
    return 0;                                     // 0 = continue, per sqlite3_exec's contract
  end OnRow;

  public method Collect(db: IntPtr, sql: string): int
    using var csql := CString.From(sql);
    var rc := sqlite3_exec(db, csql, this.OnRow, Ptr<IntPtr>.Zero);
                  // bound instance-method reference: admitted — `cb` is env-paired;
                  // four Bezel arguments: the `env` slot is compiler-supplied
    if rc <> 0 then
      throw new IOException($"sqlite3_exec failed: rc={rc}");
    end;
    return _rows;
  end Collect;
end RowCollector;
