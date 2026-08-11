public method ReportEach<T>(items: IReadOnlyList<T>) where T: class
  for var c in items do Console.WriteLine(c); end;   // Object overload -> ToString();
                                                     //   `where T: class` exposes only Object's surface (spec §14.2)
end ReportEach;

// Accepts both:
ReportEach(myArray);          // Array<Connection> → IReadOnlyList<Connection> ✓
ReportEach(myPool);            // OwningArray<Connection> → IReadOnlyList<Connection> ✓
