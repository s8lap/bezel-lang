import csv,collections,os
HERE=os.path.dirname(os.path.abspath(__file__))
DATA=os.path.dirname(HERE)
note={r['id']:r for r in csv.DictReader(open(os.path.join(DATA,'scenarios.tsv')),delimiter='\t')}
p2=dict(l.rstrip('\n').split('\t') for l in open(os.path.join(DATA,'scenarios_pass2.tsv')) if not l.startswith('id'))
m={'L':'a','B':'b','F':'c','D':'c'}
mine=dict(l.split() for l in open(os.path.join(HERE,'mine.tsv')))
pairs=[(mine[i],m[note[i]['pass1']]) for i in mine]
def kappa(pairs,cats):
    n=len(pairs);po=sum(a==b for a,b in pairs)/n
    ca=collections.Counter(a for a,_ in pairs);cb=collections.Counter(b for _,b in pairs)
    pe=sum(ca[c]*cb[c] for c in cats)/n/n
    return po,(po-pe)/(1-pe)
print('n',len(pairs),'agree/kappa',kappa(pairs,'abc'))
cm=collections.Counter(pairs)
print('rows=mine cols=note  a b c')
for x in 'abc': print(x,[cm[(x,y)] for y in 'abc'])
# 4-way with note D/F split
cm4=collections.Counter((mine[i],note[i]['pass1']) for i in mine)
print('mine vs note4',{k:v for k,v in sorted(cm4.items())})
print('\nDisagreements:')
for i in sorted(mine,key=lambda i:note[i]['condition']):
    if mine[i]!=m[note[i]['pass1']]:
        r=note[i];print(i,r['condition'],'mine',mine[i],'note',r['pass1'],'|',r['operation'],'|',r['operand_from'],'|',r['why'])
print('pass2 rows in sample',sum(1 for i in mine if i in p2))

# bug-or-not: the re-rater's F/D split of its band-(c) rows (mine_fd.txt) against the note's D
fd=dict(l.split() for l in open(os.path.join(HERE,'mine_fd.txt')))
bp=[(fd.get(i)=='D', note[i]['pass1']=='D') for i in mine]
n=len(bp);po=sum(a==b for a,b in bp)/n
pa=sum(a for a,_ in bp)/n;pb=sum(b for _,b in bp)/n;pe=pa*pb+(1-pa)*(1-pb)
print('bug-or-not n',n,'agree',po,'kappa',(po-pe)/(1-pe),dict(collections.Counter(bp)))
