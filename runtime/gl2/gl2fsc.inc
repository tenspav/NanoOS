/* On-device typed straight-line fragment frontend and exact combiner lowering.
 * Included privately by gl2api.c; serialized like the vertex compiler.
 * No sampling-based equivalence tests, source-string templates or CPU rendering.
 * Current lowering is one texture/up to two stages. Broader GLSL/execution is unfinished. */
#define F_NODES 768
#define F_TABLE 2048
#define F_SYMBOLS 48
enum { F_CONST=1,F_INPUT,F_ADD,F_MUL,F_SUB,F_CLAMP };
typedef struct {uint16_t a,b,slot;uint32_t deps;uint8_t op;float value,lo,hi;} f_node;
typedef struct {uint16_t v[4];uint8_t w,kind,index;} f_value; /* kind0 expression,1 UV,2 sampler */
typedef struct {char name[32];f_value value;uint8_t storage,slot;} f_symbol;
static struct {const char *cursor;char token[32];char *log;int error,depth;uint16_t n,table[F_TABLE];f_node nodes[F_NODES];f_symbol symbols[F_SYMBOLS];unsigned ns,nvary,nsampler,nuniform;uint8_t uniform_map[16];gl2_fs_owned *out;} f;
static void f_fail(const char *s){if(!f.error){f.error=1;scpy(f.log,s,64);}}
static int f_id(char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
static int f_digit(char c){return c>='0'&&c<='9';}
static int f_eq(const char *s){return !scmp(f.token,s);}
static void f_next(void){
 const char *s=f.cursor;unsigned n=0;
 for(;;){while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n')++s;
  if(s[0]=='/'&&s[1]=='/'){while(*s&&*s!='\n')++s;continue;}
  if(s[0]=='/'&&s[1]=='*'){s+=2;while(*s&&!(s[0]=='*'&&s[1]=='/'))++s;if(!*s){f_fail("unterminated fragment comment");break;}s+=2;continue;}break;
 }
#define F_CHAR() do{if(n>=31){f_fail("fragment token exceeds31bytes");goto done;}f.token[n++]=*s++;}while(0)
 if(f_id(*s)){while(f_id(*s)||f_digit(*s))F_CHAR();}
 else if(f_digit(*s)||(*s=='.'&&f_digit(s[1]))){while(f_digit(*s))F_CHAR();if(*s=='.'){F_CHAR();while(f_digit(*s))F_CHAR();}if(*s=='e'||*s=='E'){F_CHAR();if(*s=='+'||*s=='-')F_CHAR();if(!f_digit(*s))f_fail("missing fragment exponent");while(f_digit(*s))F_CHAR();}}
 else if(*s){const char *p="+-*/=;(),.{}";while(*p&&*p!=*s)++p;if(!*p)f_fail("unsupported fragment character");F_CHAR();}
 done:f.token[n]=0;f.cursor=s;
#undef F_CHAR
}
static void f_eat(const char *s){if(!f_eq(s)){f_fail("unexpected fragment token");return;}f_next();}
static uint32_t f_bits(float x){union{float x;uint32_t u;}v={x};return v.u;}
static uint16_t f_intern(unsigned op,unsigned a,unsigned b,float value){
 uint32_t hash=(op*0x9e3779b9u)^(a*0x85ebca6bu)^(b*0xc2b2ae35u)^f_bits(value);unsigned slot=hash&(F_TABLE-1);
 while(f.table[slot]){unsigned i=f.table[slot];f_node *n=&f.nodes[i];if(n->op==op&&n->a==a&&n->b==b&&f_bits(n->value)==f_bits(value))return(uint16_t)i;slot=(slot+1)&(F_TABLE-1);}
 if(f.n+1>=F_NODES){f_fail("fragment expression exceeds node limit");return 1;}
 unsigned i=++f.n;f_node *n=&f.nodes[i];n->op=(uint8_t)op;n->a=(uint16_t)a;n->b=(uint16_t)b;n->value=value;n->slot=(uint16_t)slot;n->deps=op==F_INPUT?(1u<<(a<8?a:8+(a-8)/4)):op==F_CONST?0:(f.nodes[a].deps|(op==F_CLAMP?0:f.nodes[b].deps));
 if(op==F_CONST)n->lo=n->hi=value;
 else if(op==F_INPUT){n->lo=0;n->hi=1;}
 else if(op==F_CLAMP){n->lo=0;n->hi=1;}
 else{f_node *x=&f.nodes[a],*y=&f.nodes[b];if(op==F_ADD){n->lo=x->lo+y->lo;n->hi=x->hi+y->hi;}else if(op==F_SUB){n->lo=x->lo-y->hi;n->hi=x->hi-y->lo;}else{float v[4]={x->lo*y->lo,x->lo*y->hi,x->hi*y->lo,x->hi*y->hi};n->lo=n->hi=v[0];for(unsigned k=1;k<4;++k){if(v[k]<n->lo)n->lo=v[k];if(v[k]>n->hi)n->hi=v[k];}}}
 f.table[slot]=(uint16_t)i;return(uint16_t)i;
}
static uint16_t f_const(float v){if((f_bits(v)&0x7f800000u)==0x7f800000u){f_fail("fragment constant out of range");return 1;}return f_intern(F_CONST,0,0,v==0?0:v);}
static int f_is(uint16_t id,float v){return f.nodes[id].op==F_CONST&&f.nodes[id].value==v;}
static unsigned f_terms(unsigned op,uint16_t id,uint16_t *terms,unsigned count){
 if(f.nodes[id].op==op){count=f_terms(op,f.nodes[id].a,terms,count);return f_terms(op,f.nodes[id].b,terms,count);}
 if(count>=32){f_fail("fragment arithmetic exceeds term limit");return count;}terms[count++]=id;return count;
}
static uint16_t f_op(unsigned op,uint16_t a,uint16_t b){
 if(f.nodes[a].op==F_CONST&&f.nodes[b].op==F_CONST){float x=f.nodes[a].value,y=f.nodes[b].value;return f_const(op==F_ADD?x+y:op==F_MUL?x*y:x-y);}
 if(op==F_SUB){if(a==b)return 1;if(f_is(b,0))return a;return f_intern(op,a,b,0);}
 if(op==F_ADD){if(f_is(a,0))return b;if(f_is(b,0))return a;}
 if(op==F_MUL){if(f_is(a,0)||f_is(b,0))return 1;if(f_is(a,1))return b;if(f_is(b,1))return a;}
 uint16_t terms[32];unsigned n=f_terms(op,a,terms,0);n=f_terms(op,b,terms,n);
 for(unsigned i=1;i<n;++i){uint16_t v=terms[i];unsigned j=i;while(j&&terms[j-1]>v){terms[j]=terms[j-1];--j;}terms[j]=v;}
 uint16_t result=terms[0];for(unsigned i=1;i<n;++i)result=f_intern(op,result,terms[i],0);return result;
}
static f_value f_zero(void){f_value v={{1,1,1,1},1,0,0};return v;}
static f_value f_binary(unsigned op,f_value a,f_value b){
 if(a.kind||b.kind){f_fail("fragment arithmetic on sampler/UV unsupported");return f_zero();}
 if(a.w!=b.w&&a.w!=1&&b.w!=1){f_fail("fragment vector width mismatch");return f_zero();}
 f_value v=f_zero();v.w=a.w>b.w?a.w:b.w;for(unsigned i=0;i<v.w;++i)v.v[i]=f_op(op,a.v[a.w==1?0:i],b.v[b.w==1?0:i]);return v;
}
static int f_width(const char *s){return !scmp(s,"float")?1:!scmp(s,"vec2")?2:!scmp(s,"vec3")?3:!scmp(s,"vec4")?4:0;}
static int f_find(const char *s){for(unsigned i=0;i<f.ns;++i)if(!scmp(s,f.symbols[i].name))return(int)i;return -1;}
static int f_valid_name(const char *name){
 static const char *const keywords[]={"attribute","const","uniform","varying","precision","lowp","mediump","highp","float","vec2","vec3","vec4","void","main","int","bool","sampler2D","if","else","for","while","return","discard","struct","in","out","inout"};
 if(!f_id(name[0])||(name[0]=='g'&&name[1]=='l'&&name[2]=='_'))return 0;
 for(unsigned i=0;i<sizeof keywords/sizeof *keywords;++i)if(!scmp(name,keywords[i]))return 0;
 return 1;
}
static void f_name(char *s){if(!f_valid_name(f.token)){f_fail("invalid fragment declaration name");s[0]=0;return;}scpy(s,f.token,32);f_next();}
static float f_number(void){
 const char *s=f.token;float v=0,unit=1;int dot=0;
 for(;*s&&*s!='e'&&*s!='E';++s){if(*s=='.')dot=1;else if(dot){unit*=.1f;v+=(*s-'0')*unit;}else v=v*10+(*s-'0');}
 if(*s){++s;int sign=1,e=0;if(*s=='+'||*s=='-'){if(*s=='-')sign=-1;++s;}while(f_digit(*s)){e=e*10+(*s++-'0');if(e>255){f_fail("fragment exponent out of range");return 0;}}while(e--)v=sign>0?v*10:v/10;}
 return v;
}
static f_value f_expr(void);
static f_value f_primary(void){
 f_value v=f_zero();
 if(f_eq("(")){f_next();v=f_expr();f_eat(")");}
 else if(f_digit(f.token[0])||(f.token[0]=='.'&&f_digit(f.token[1]))){v.v[0]=f_const(f_number());f_next();}
 else{char name[32];scpy(name,f.token,32);f_next();
  if(f_eq("(")){f_next();f_value args[8];unsigned count=0;if(!f_eq(")"))for(;;){if(count>=8){f_fail("too many fragment arguments");break;}args[count++]=f_expr();if(!f_eq(","))break;f_next();}f_eat(")");int width=f_width(name);
   if(width){v.w=(uint8_t)width;unsigned n=0;for(unsigned a=0;a<count;++a){if(args[a].kind){f_fail("sampler/UV fragment constructor unsupported");break;}for(unsigned k=0;k<args[a].w;++k)if(n<4)v.v[n++]=args[a].v[k];else++n;}
    if(count==1&&args[0].w==1){for(int k=0;k<width;++k)v.v[k]=args[0].v[0];}
    else if(n<(unsigned)width||(n>(unsigned)width&&count!=1))f_fail("fragment constructor width mismatch");
   }else if(!scmp(name,"texture2D")){if(count!=2||args[0].kind!=2||args[1].kind!=1||args[1].w!=2){f_fail("texture2D needs direct sampler and UV varying");return v;}if(args[0].index||args[1].index!=3){f_fail("fragment lowering currently needs texture0/UV0");return v;}v.w=4;for(unsigned k=0;k<4;++k)v.v[k]=f_intern(F_INPUT,4+k,0,0);}
   else if(!scmp(name,"dot")){if(count!=2||args[0].kind||args[1].kind||args[0].w!=args[1].w){f_fail("fragment dot argument mismatch");return v;}v=f_zero();for(unsigned k=0;k<args[0].w;++k)v.v[0]=f_op(F_ADD,v.v[0],f_op(F_MUL,args[0].v[k],args[1].v[k]));}
    else if(!scmp(name,"mix")){if(count!=3||args[0].w!=args[1].w||args[0].kind||args[1].kind||args[2].kind||(args[2].w!=1&&args[2].w!=args[0].w)){f_fail("fragment mix argument mismatch");return v;}f_value one=f_zero();one.v[0]=f_const(1);v=f_binary(F_ADD,f_binary(F_MUL,args[0],f_binary(F_SUB,one,args[2])),f_binary(F_MUL,args[1],args[2]));}
   else if(!scmp(name,"clamp")){if(count!=3||args[0].kind||args[1].kind||args[2].kind||(args[1].w!=1&&args[1].w!=args[0].w)||(args[2].w!=1&&args[2].w!=args[0].w)){f_fail("fragment clamp argument mismatch");return v;}v=args[0];for(unsigned k=0;k<v.w;++k){if(!f_is(args[1].v[args[1].w==1?0:k],0)||!f_is(args[2].v[args[2].w==1?0:k],1)){f_fail("fragment clamp needs0and1 limits");break;}v.v[k]=f_intern(F_CLAMP,v.v[k],0,0);}}
   else f_fail("fragment builtin needs broader execution path");
  }else{int i=f_find(name);if(i<0)f_fail("unknown fragment identifier");else v=f.symbols[i].value;}
 }
 while(f_eq(".")&&!f.error){f_next();if(v.kind){f_fail("UV/sampler swizzle unsupported");break;}f_value out=f_zero();out.w=0;int set=-1;
  for(const char *s=f.token;*s;++s){const char *sets[3]={"xyzw","rgba","stpq"};int lane=-1,which=-1;for(int q=0;q<3;++q)for(int k=0;k<4;++k)if(sets[q][k]==*s){lane=k;which=q;}
   if(lane<0||lane>=v.w||out.w>=4||(set>=0&&set!=which)){f_fail("invalid fragment swizzle");break;}set=which;out.v[out.w++]=v.v[lane];}
  if(!out.w){f_fail("empty fragment swizzle");}v=out;f_next();
 }
 return v;
}
static f_value f_unary(void){if(++f.depth>32){f_fail("fragment nesting exceeds32");--f.depth;return f_zero();}f_value v;if(f_eq("-")||f_eq("+")){int neg=f_eq("-");f_next();v=f_unary();if(neg){f_value zero=f_zero();v=f_binary(F_SUB,zero,v);}}else v=f_primary();--f.depth;return v;}
static f_value f_product(void){f_value v=f_unary();while(!f.error&&(f_eq("*")||f_eq("/"))){int div=f_eq("/");f_next();f_value b=f_unary();if(div){if(b.kind){f_fail("fragment division by sampler/UV");break;}for(unsigned k=0;k<b.w;++k){f_node *n=&f.nodes[b.v[k]];if(n->op!=F_CONST||!n->value){f_fail("fragment division needs nonzero constants");break;}b.v[k]=f_const(1/n->value);}}v=f_binary(F_MUL,v,b);}return v;}
static f_value f_expr(void){f_value v=f_product();while(!f.error&&(f_eq("+")||f_eq("-"))){unsigned op=f_eq("+")?F_ADD:F_SUB;f_next();v=f_binary(op,v,f_product());}return v;}
static void f_precision(void){if(f_eq("lowp")||f_eq("mediump")||f_eq("highp"))f_next();}
static void f_symbol_add(const char *name,f_value value,unsigned storage,unsigned slot){if(!f_valid_name(name)){f_fail("reserved fragment declaration name");return;}if(f_find(name)>=0){f_fail("duplicate fragment symbol");return;}if(f.ns>=F_SYMBOLS){f_fail("too many fragment symbols");return;}f_symbol *s=&f.symbols[f.ns++];scpy(s->name,name,32);s->value=value;s->storage=(uint8_t)storage;s->slot=(uint8_t)slot;}
static f_value f_parse(const char *source){
 f.cursor=source;f_next();while(!f.error&&!f_eq("void")&&f.token[0]){
  if(f_eq("precision")){f_next();if(!f_eq("lowp")&&!f_eq("mediump")&&!f_eq("highp")){f_fail("missing fragment precision qualifier");break;}f_precision();if(!f_eq("float")&&!f_eq("int")&&!f_eq("sampler2D")){f_fail("invalid fragment precision type");break;}f_next();f_eat(";");continue;}
  unsigned storage=f_eq("uniform")?2:f_eq("varying")?1:0;if(!storage){f_fail("unsupported fragment global declaration");break;}f_next();f_precision();int sampler=f_eq("sampler2D"),width=f_width(f.token);f_next();char name[32];f_name(name);f_eat(";");f_value v=f_zero();unsigned slot=0;
  if(storage==2){if(sampler){if(f.nsampler>=1){f_fail("fragment compiler currently supports one sampler");break;}v.kind=2;v.index=(uint8_t)f.nsampler++;}
   else{if(!width){f_fail("unsupported fragment uniform type");break;}if(f.nuniform>=16){f_fail("fragment uniform limit16");break;}v.w=(uint8_t)width;v.index=(uint8_t)f.nuniform++;storage=3;for(int k=0;k<width;++k)v.v[k]=f_intern(F_INPUT,GL2_SOFT_UNIFORM_BASE+4*v.index+(unsigned)k,0,0);}
  }
  else{if(sampler||(width!=2&&width!=4)){f_fail("unsupported fragment varying type");break;}v.w=(uint8_t)width;if(width==4){slot=1;for(unsigned i=0;i<f.ns;++i)if(f.symbols[i].storage==1&&f.symbols[i].slot==slot)f_fail("fragment colour varying slot occupied");for(unsigned k=0;k<4;++k)v.v[k]=f_intern(F_INPUT,k,0,0);}
   else{slot=3;for(unsigned i=0;i<f.ns;++i)if(f.symbols[i].storage==1&&f.symbols[i].slot==3)slot=4;v.kind=1;v.index=(uint8_t)slot;}
   if((!scmp(name,"vColor")&&width!=4)||((!scmp(name,"vTexCoord")||!scmp(name,"vTexCoord1"))&&width!=2)){f_fail("reserved fragment varying type mismatch");}if(!scmp(name,"vTexCoord1"))slot=v.index=4;if(!scmp(name,"vTexCoord")&&slot!=3)f_fail("fragment varying slot collision");
   for(unsigned i=0;i<f.ns;++i)if(f.symbols[i].storage==1&&f.symbols[i].slot==slot)f_fail("fragment varying slot occupied");
  }f_symbol_add(name,v,storage,slot);
 }
 f_eat("void");f_eat("main");f_eat("(");if(f_eq("void"))f_next();f_eat(")");f_eat("{");f_value output=f_zero();unsigned assigned=0;
 while(!f.error&&!f_eq("}")&&f.token[0]){f_precision();int width=f_width(f.token);char name[32];if(width)f_next();scpy(name,f.token,32);if(!f_id(name[0])){f_fail("invalid fragment statement");break;}f_next();f_eat("=");f_value v=f_expr();f_eat(";");if(!scmp(name,"gl_FragColor")){if(width||v.kind||v.w!=4){f_fail("gl_FragColor needs vec4");break;}output=v;assigned=1;}
  else if(width){if(v.kind||v.w!=width){f_fail("fragment local type mismatch");break;}f_symbol_add(name,v,0,0);}
  else{int i=f_find(name);if(i<0||f.symbols[i].storage||v.kind||v.w!=f.symbols[i].value.w){f_fail("invalid fragment assignment");break;}f.symbols[i].value=v;}
 }
 f_eat("}");if(f.token[0])f_fail("fragment text after main");if(!assigned)f_fail("gl_FragColor not assigned");return output;
}
static void f_rollback(unsigned mark){while(f.n>mark){f.table[f.nodes[f.n].slot]=0;--f.n;}}
static uint16_t f_previous[4];
static uint16_t f_source(unsigned code,unsigned alpha,unsigned channel){if(code==1)return f_previous[alpha?3:channel];if(code==3)return 1;return f_intern(F_INPUT,(code==2?4:0)+(alpha?3:channel),0,0);}
static uint16_t f_stage_base(const uint8_t *c,unsigned channel,int alpha){
 uint16_t one=f_const(1),x1=c[0]==3?one:f_source(c[0],0,channel),y1=f_source(c[2],c[3]||alpha,channel);
 if(alpha&&c[0]!=3)x1=f_source(c[0],1,channel);
 if(c[1]){x1=f_op(F_SUB,one,x1);}if(c[4]){y1=f_op(F_SUB,one,y1);}uint16_t out=f_op(F_MUL,x1,y1);
 if(!c[9]){uint16_t x2=c[5]==3?one:f_source(c[5],alpha,channel),y2=f_source(c[6],c[7]||alpha,channel);if(c[8])y2=f_op(F_SUB,one,y2);out=f_op(F_ADD,out,f_op(F_MUL,x2,y2));}
 return out;
}
/* Exhaust finite instruction operands; exact hash-consed AST identity is required.
 * Commutative/associative normalization handles reordered arithmetic. Candidate
 * nodes are rolled back; no numerical probes or hash-only acceptance. */
/* Strip instruction post-modifiers from the target before enumeration.
 * This is structural inversion, not algebraic approximation. It removes an
 * eightfold bias/scale loop for the overwhelmingly common unmodified target. */
static uint16_t f_unmodify(uint16_t id,unsigned bias,unsigned scale){
 if(scale==3){f_node n=f.nodes[id];if(n.op!=F_SUB||!f_is(n.a,1))return 0;id=n.b;}
 else if(scale){
  uint16_t terms[32];unsigned n=f_terms(F_MUL,id,terms,0),take=32;
  for(unsigned i=0;i<n;++i)if(f_is(terms[i],scale==1?2:4)){take=i;break;}
  if(take==32)return 0;
  id=f_const(1);for(unsigned i=0;i<n;++i)if(i!=take)id=f_op(F_MUL,id,terms[i]);
 }
 if(bias){f_node n=f.nodes[id];if(n.op!=F_SUB||!f_is(n.b,.5f))return 0;id=n.a;}
 return id;
}
static int f_lower(f_value target,uint8_t *plan,int alpha,int previous){
 const unsigned xs[4]={2,3,previous?1u:0u,0},ys[4]={0,2,3,1};
 unsigned outer=f.n,channels=alpha?1:3;
 for(unsigned bias=0;bias<2;++bias)for(unsigned scale=0;scale<4;++scale){
  f_value base=target;int valid=1;
  for(unsigned k=0;k<channels;++k){unsigned lane=alpha?3:k;base.v[lane]=f_unmodify(target.v[lane],bias,scale);if(!base.v[lane])valid=0;}
  if(!valid){f_rollback(outer);continue;}
  unsigned mark=f.n;uint8_t c[12]={0};c[10]=(uint8_t)bias;c[11]=(uint8_t)scale;
  for(unsigned products=1;products<=2;++products)
  for(unsigned xi=0;xi<(previous?(alpha?4u:3u):alpha?3u:2u);++xi)for(c[1]=0;c[1]<2;++c[1])for(unsigned yi=0;yi<(previous?4u:3u);++yi)for(c[3]=0;c[3]<(alpha?1:2);++c[3])for(c[4]=0;c[4]<2;++c[4])for(unsigned term=products==1?36:0;term<(products==1?37u:36u);++term){
   c[0]=(uint8_t)xs[xi];c[2]=(uint8_t)ys[yi];c[9]=term==36;c[5]=term==36?3:(uint8_t)(term/12==0?0:term/12==1?2:3);c[6]=term==36?0:(uint8_t)ys[(term%12)/4];c[7]=alpha?0:(uint8_t)((term%4)/2);c[8]=term==36?0:(uint8_t)(term&1);
   int match=1;for(unsigned k=0;k<channels&&match;++k)match=f_stage_base(c,alpha?3:k,alpha)==base.v[alpha?3:k];
   f_rollback(mark);if(f.error)return -1;if(match){for(unsigned k=0;k<12;++k)plan[k]=c[k];f_rollback(outer);return 0;}
  }
  f_rollback(outer);
 }
 return -1;
}
/* Bounded two-stage graph search. Candidate intermediates come from the
 * source DAG and associative product pairs, not source-expression templates.
 * Only texture-bounded values may cross the stage clamp. Primary/uniform
 * ranges are not a proof of a GLSL value bound. RGB and alpha are rematched
 * independently using the same finite instruction grammar as one-stage code. */
#define F_STAGE_CANDIDATES 16
static uint16_t f_lane(uint16_t id,unsigned lane){
 f_node n=f.nodes[id];
 if(n.op==F_CONST)return id;
 if(n.op==F_INPUT)return n.a==4?f_intern(F_INPUT,4+lane,0,0):0;
 uint16_t a=f_lane(n.a,lane);if(!a)return 0;
 if(n.op==F_CLAMP)return f_intern(F_CLAMP,a,0,0);
 uint16_t b=f_lane(n.b,lane);return b?f_op(n.op,a,b):0;
}
static void f_candidate(uint16_t id,uint16_t *ids,unsigned *count){
 f_node *n=&f.nodes[id];
 if(n->op==F_CONST||n->op==F_INPUT||(n->deps&~0x10u)||n->lo<0||n->hi>1)return;
 for(unsigned i=0;i<*count;++i)if(ids[i]==id)return;
 if(*count<F_STAGE_CANDIDATES)ids[(*count)++]=id;
}
static unsigned f_candidates(uint16_t root,unsigned channel,uint16_t *ids){
 unsigned count=0,original=f.n;uint8_t live[F_NODES]={0};live[root]=1;
 for(unsigned i=original;i;--i)if(live[i]){
  f_node n=f.nodes[i];
  if(channel==0)f_candidate((uint16_t)i,ids,&count);
  else if(n.op!=F_CONST&&n.op!=F_INPUT&&!(n.deps&~0x80u)&&n.lo>=0&&n.hi<=1&&count<F_STAGE_CANDIDATES)ids[count++]=(uint16_t)i;
  if(n.op!=F_CONST&&n.op!=F_INPUT){live[n.a]=1;if(n.op!=F_CLAMP)live[n.b]=1;}
 }
 for(unsigned i=1;i<=original&&count<F_STAGE_CANDIDATES;++i)if(live[i]&&f.nodes[i].op==F_MUL){
  uint16_t terms[32];unsigned n=f_terms(F_MUL,(uint16_t)i,terms,0);
  for(unsigned a=0;a<n&&count<F_STAGE_CANDIDATES;++a)for(unsigned b=a+1;b<n&&count<F_STAGE_CANDIDATES;++b){
   if((f.nodes[terms[a]].deps|f.nodes[terms[b]].deps)&~(1u<<(4+channel)))continue;
   uint16_t id=f_op(F_MUL,terms[a],terms[b]);
   if(channel==0)f_candidate(id,ids,&count);
   else if(f.nodes[id].lo>=0&&f.nodes[id].hi<=1){unsigned j=0;for(;j<count;++j)if(ids[j]==id)break;if(j==count)ids[count++]=id;}
  }
 }
 return count;
}
static int f_lower_two(f_value target,uint8_t plan[2][12],uint8_t alpha[2][12]){
 uint16_t ids[F_STAGE_CANDIDATES+1],aids[F_STAGE_CANDIDATES+2];
 unsigned count=f_candidates(target.v[0],0,ids),acount=f_candidates(target.v[3],3,aids);
 /* Pass-through is essential when only alpha requires two stages. */
 ids[count++]=f_intern(F_INPUT,4,0,0);aids[acount++]=f_intern(F_INPUT,7,0,0);
 for(unsigned i=0;i<count&&!f.error;++i){
  unsigned mark=f.n;f_value first=f_zero();first.w=4;int mapped=1;
  for(unsigned k=0;k<4;++k){first.v[k]=f_lane(ids[i],k);if(!first.v[k])mapped=0;}
  if(mapped&&!f_lower(first,plan[0],0,0)){
   /* Add the corresponding alpha shape as the first choice for backward
    * precision stability; then independently enumerate alpha source DAGs. */
   uint16_t shared=first.v[3];
   for(unsigned mode=0;mode<=acount;++mode){
    first.v[3]=mode?aids[mode-1]:shared;
    if(f_lower(first,alpha[0],1,0))continue;
    for(unsigned k=0;k<4;++k)f_previous[k]=first.v[k];
    if(!f_lower(target,plan[1],0,1)&&!f_lower(target,alpha[1],1,1))return 0;
   }
  }
  f_rollback(mark);
 }
 return -1;
}
/* DOT3 implements 4*sum((X-.5)*(Y-.5)); match that AST exactly.
 * Do not rewrite arbitrary dot/normalize into biased fixed-function maths. */
#ifdef GL2_EXPERIMENTAL_DOT3
static int f_lower_dot3(f_value target,uint8_t *plan,uint8_t *alpha){
 unsigned mark=f.n;
 for(unsigned y=0;y<2;++y){
  uint16_t d=f_const(0);
  for(unsigned k=0;k<3;++k){uint16_t a=f_intern(F_INPUT,4+k,0,0),b=f_intern(F_INPUT,(y?0:4)+k,0,0);d=f_op(F_ADD,d,f_op(F_MUL,f_op(F_SUB,a,f_const(.5f)),f_op(F_SUB,b,f_const(.5f))));}
  d=f_op(F_MUL,d,f_const(4));
  int match=target.v[0]==d&&target.v[1]==d&&target.v[2]==d;
  f_rollback(mark);if(f.error)return -1;
  if(match){
   /* Candidate texture X1 and primary/texture Y1 from decoded traces.
    * Primary X1 is reserved until independently verified on hardware. */
   for(unsigned i=0;i<12;++i)plan[i]=0;
   plan[0]=2;plan[2]=y?0:2;plan[9]=1;
   return f_lower(target,alpha,1,0);
  }
 }
 return -1;
}
#endif
static uint16_t f_alpha_word(const uint8_t *a){return(uint16_t)(a[0]<<13|a[2]<<11|a[5]<<9|a[6]<<7|a[1]<<6|a[4]<<5|a[8]<<3|a[9]<<4|a[10]<<2|a[11]);}
static unsigned f_dependencies(uint16_t id){return f.nodes[id].deps;}
static int f_software(f_value target,gl2_soft_program *p){
 uint8_t live[F_NODES]={0};uint16_t map[F_NODES];
 for(unsigned k=0;k<4;++k)live[target.v[k]]=1;
 for(unsigned i=f.n;i;--i)if(live[i]){f_node *n=&f.nodes[i];if(n->op!=F_CONST&&n->op!=F_INPUT){live[n->a]=1;if(n->op!=F_CLAMP)live[n->b]=1;}}
 p->count=0;for(unsigned i=1;i<=f.n;++i)if(live[i]){if(p->count>=GL2_SOFT_INSNS){f_fail("fragment software instruction limit");return -1;}f_node *n=&f.nodes[i];map[i]=p->count;gl2_soft_insn *s=&p->code[p->count++];s->op=n->op;s->value=n->value;s->a=n->op==F_INPUT?(n->a<GL2_SOFT_UNIFORM_BASE?n->a:GL2_SOFT_UNIFORM_BASE+f.uniform_map[(n->a-GL2_SOFT_UNIFORM_BASE)/4]*4+(n->a-GL2_SOFT_UNIFORM_BASE)%4):n->op==F_CONST?0:map[n->a];s->b=(n->op==F_ADD||n->op==F_MUL||n->op==F_SUB)?map[n->b]:0;}
 for(unsigned k=0;k<4;++k){p->output[k]=map[target.v[k]];}return 0;
}
static int compile_fragment(const char *source,gl2_fs_owned *out,char *log){
 f.cursor=0;f.n=f.ns=f.nvary=f.nsampler=f.nuniform=f.depth=f.error=0;f.log=log;f.out=out;for(unsigned i=0;i<F_TABLE;++i)f.table[i]=0;
 for(unsigned i=0;i<sizeof *out;++i)((uint8_t *)out)[i]=0;
 f_const(0);f_const(1);f_value target=f_parse(source);if(f.error)return -1;
 for(unsigned k=0;k<4;++k)if(f.nodes[target.v[k]].op==F_CLAMP)target.v[k]=f.nodes[target.v[k]].a;
 unsigned deps=0;for(unsigned k=0;k<4;++k)deps|=f_dependencies(target.v[k]);
 for(unsigned i=0;i<f.ns;++i){f_symbol *s=&f.symbols[i];if(s->storage==3&&(deps&(1u<<(8+s->value.index)))){unsigned u=out->e.nuniform++;f.uniform_map[s->value.index]=(uint8_t)u;scpy(out->uniforms[u],s->name,32);out->e.uniform_names[u]=out->uniforms[u];out->e.uniform_widths[u]=s->value.w;}}
 if(f_software(target,&out->software)){return -1;}out->e.software=&out->software;
 uint8_t alpha[2][12]={{0}};int gpu=0;for(unsigned k=0;k<4;++k)f_previous[k]=1;
 if((deps&0xffu)&&!(deps&~255u)){
  /* Physical probe354 did not match DOT3 source arithmetic. Keep the
   * candidate encoding diagnostic-only until operands/layout are resolved. */
#ifdef GL2_EXPERIMENTAL_DOT3
  if(!f_lower_dot3(target,out->e.plan[0],alpha[0])){gpu=1;out->e.has_alpha=2;}
#else
  if(0){gpu=1;}
#endif
  else if(!f_lower(target,out->e.plan[0],0,0)&&!f_lower(target,alpha[0],1,0))gpu=1;
  else if(!f.error&&!f_lower_two(target,out->e.plan,alpha))gpu=2;

 }f.error=0;
 out->e.hash=gl2_hash(source);out->e.source=source;out->e.nsampler=(deps&0xf0u)?1:0;out->e.nplan=(uint8_t)gpu;out->e.has_alpha|=(uint8_t)(gpu!=0);
 for(unsigned stage=0;stage<(unsigned)gpu;++stage)out->e.alpha[stage]=f_alpha_word(alpha[stage]);
 for(unsigned i=0;i<f.ns;++i){f_symbol *s=&f.symbols[i];if(s->storage==2&&out->e.nsampler){scpy(out->samplers[0],s->name,32);out->e.sampler_names[0]=out->samplers[0];}
  else if(s->storage==1&&((s->slot==1&&(deps&15u))||(s->slot==3&&out->e.nsampler))){unsigned n=out->e.nvary;if(n>=3){f_fail("too many fragment varyings");return -1;}scpy(out->varyings[n],s->name,32);out->e.vary_names[n]=out->varyings[n];out->e.vary_slots[n]=s->slot;out->e.vary_widths[n]=s->value.w;++out->e.nvary;if(s->slot==1)out->e.needs_colour=1;}
 }
 scpy(log,gpu?"compiled:native combiner plan; state may require fallback":"compiled:software-only; no native plan (slow)",64);return 0;
}
