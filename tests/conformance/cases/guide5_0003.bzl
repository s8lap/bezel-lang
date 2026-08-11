method ReportEach(items: IEnumerable<Connection>)
  for var c in items do Console.WriteLine(c.Name); end;
end ReportEach;

ReportEach(index);     // Array<Connection>: fine
ReportEach(pool);      // OwningArray<Connection>: fine — iteration borrows either way
