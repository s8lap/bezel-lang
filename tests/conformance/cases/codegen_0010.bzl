var max   := (if a > b then a else b);                    // `else` MANDATORY
var grade := (if s >= 90 then "A" elif s >= 80 then "B" else "F");
var g2    := (case score of 90..100 => "A", 70..<80 => "C", _ => "F");
var g3    := (if score >= 90 then "A" else "F");
var area  := (var w := bounds.Width(); w * w);            // binding atom (§17.15)
