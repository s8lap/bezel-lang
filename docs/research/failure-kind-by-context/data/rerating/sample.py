import csv,random,collections,math,os,sys
HERE=os.path.dirname(os.path.abspath(__file__))
OUT=sys.argv[1] if len(sys.argv)>1 else os.path.join(HERE,'sample.tsv')
random.seed(20261111)
rows=list(csv.DictReader(open(os.path.join(HERE,'blind.tsv')),delimiter='\t'))
by=collections.defaultdict(list)
for r in rows: by[r['condition']].append(r)
out=[]
for c in sorted(by):
    g=by[c]
    k = max(3, math.ceil(len(g)*0.30)) if len(g)>=5 else 1
    out += random.sample(g,k)
out.sort(key=lambda r:(r['condition'],r['id']))
with open(OUT,'w') as f:
    for r in out: f.write('\t'.join([r['id'],r['condition'],r['domain'],r['task'],r['operation']])+'\n')
print(len(out))
