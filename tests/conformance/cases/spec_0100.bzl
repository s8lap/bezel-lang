var grade := (when
  score >= 90 => "A",
  score >= 80 => "B",
  score >= 70 => "C",
  _ => "F"
);
