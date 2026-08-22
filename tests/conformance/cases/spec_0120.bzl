var grade := (case score of
  90..100 => "A",
  _ => "F");             // the atom's ')' closes the case; ';' ends the declaration

case month of
  1 => Console.WriteLine("Jan"),
  _ => Console.WriteLine("Other")
end;                    // statement position: 'end' closes the case; ';' the statement
