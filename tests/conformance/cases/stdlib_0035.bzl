public method ReportEach<T>(items: IReadOnlyList<T>) where T: class
  for var c in items do Console.WriteLine(c.Name); end;
end ReportEach;

// Accepts both:
ReportEach(myArray);          // Array<Connection> → IReadOnlyList<Connection> ✓
ReportEach(myPool);            // OwningArray<Connection> → IReadOnlyList<Connection> ✓
