// Source:                         // Desugars to:
counts[category]++;                do
                                     var __coll := counts;
                                     var __idx := category;
                                     __coll[__idx] := __coll[__idx] + 1;
                                   end;
