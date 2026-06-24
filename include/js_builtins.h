/* ===========================================================================
 *  BoltJS built-ins -- included at the tail of kernel/js.c (shares its statics).
 *  Member access (.length, properties, DOM fields) and method dispatch for
 *  strings, arrays, and the host singletons console / Math / document / JSON,
 *  plus the global functions a page script reaches for.
 * ===========================================================================*/

#define JS_TARGET_DOC ((js_dom_node)(uintptr_t)1)   /* sentinel: document/window */
static JVal wrap_dom(Interp *I, js_dom_node d) {
    if (!d) return jnull();
    JObj *o = new_obj(I, K_OBJ); if (!o) return jnull();
    o->dom = d; return jobj(o);
}
/* register an event listener (target may be JS_TARGET_DOC) */
static void js_add_listener(Interp *I, js_dom_node tgt, const char *type, JVal fn) {
    if (I->nlisteners >= 64 || fn.t != T_OBJ) return;
    if (fn.u.obj->kind != K_FUN && fn.u.obj->kind != K_NATIVE) return;
    int id = I->nlisteners++;
    I->listeners[id].target = tgt; I->listeners[id].fn = fn.u.obj; I->listeners[id].active = 1;
    uint32_t k = 0; for (; type[k] && k < 19; k++) I->listeners[id].type[k] = type[k]; I->listeners[id].type[k] = 0;
}
static char *join_args(Interp *I, JVal *a, int n, const char *sep) {
    char *s = ""; for (int i=0;i<n;i++){ if(i) s=str_concat(I,s,sep); s=str_concat(I,s,val_to_str(I,a[i])); } return s;
}
static char *substr_dup(Interp *I, const char *s, int start, int len) {
    if (start<0) start=0; int L=strlen(s); if(start>L)start=L;
    if (len<0) len=0; if (start+len>L) len=L-start;
    return arena_str(&I->a, s+start, len);
}
static int str_index_of(const char *hay, const char *needle, int from) {
    int hl=strlen(hay), nl=strlen(needle); if(nl==0) return from<=hl?from:hl;
    for (int i=from<0?0:from; i+nl<=hl; i++){ int j=0; for(;j<nl;j++) if(hay[i+j]!=needle[j])break; if(j==nl)return i; }
    return -1;
}

/* ===========================================================================
 *  Promises + fetch().  The engine is synchronous, so fetch() performs the HTTP
 *  GET inline and immediately *settles* the promise; .then/.catch reactions are
 *  deferred onto a microtask FIFO that the VM drains after each turn (script
 *  eval, timer pump, event dispatch).  This gives correct ordering ("then runs
 *  after the current script finishes") without a real event loop.
 *
 *  A promise is a K_OBJ with htag H_PROMISE and hidden props:
 *    __state  0 pending / 1 fulfilled / 2 rejected
 *    __value  the result (or rejection reason)
 *    __rxs    array of pending reaction records {f,r,p} (handlers + result promise)
 * ===========================================================================*/
static int  js_prom_state(JObj *p){ JVal v; return obj_get(p,"__state",&v)?(int)v.u.num:0; }
static JVal js_prom_value(JObj *p){ JVal v; return obj_get(p,"__value",&v)?v:jundef(); }

static JObj *js_new_promise(Interp *I){
    JObj *p=new_obj(I,K_OBJ); if(!p)return 0; p->htag=H_PROMISE;
    obj_set(I,p,"__state",jnum(0));
    JObj *rxs=new_obj(I,K_ARR); if(rxs) obj_set(I,p,"__rxs",jobj(rxs));
    return p;
}
static void js_mq_push(Interp *I, JObj *onF, JObj *onR, JObj *rp, int state, JVal val){
    if(I->mq_count>=128) return;
    int i=(I->mq_head+I->mq_count)%128;
    I->microq[i].onF=onF; I->microq[i].onR=onR; I->microq[i].rp=rp;
    I->microq[i].state=state; I->microq[i].val=val; I->mq_count++;
}
static void js_settle(Interp *I, JObj *p, int state, JVal val){
    if(!p || js_prom_state(p)!=0) return;            /* null or already settled */
    obj_set(I,p,"__state",jnum(state));
    obj_set(I,p,"__value",val);
    JVal rxv; if(obj_get(p,"__rxs",&rxv) && rxv.t==T_OBJ){
        JObj *rxs=rxv.u.obj;
        for(int i=0;i<rxs->len;i++){ if(rxs->items[i].t!=T_OBJ)continue; JObj *rx=rxs->items[i].u.obj; if(!rx)continue;
            JVal f=jundef(),r=jundef(),rp=jundef(); obj_get(rx,"f",&f); obj_get(rx,"r",&r); obj_get(rx,"p",&rp);
            js_mq_push(I, f.t==T_OBJ?f.u.obj:0, r.t==T_OBJ?r.u.obj:0, rp.t==T_OBJ?rp.u.obj:0, state, val);
        }
        rxs->len=0;
    }
}
/* resolve p with an arbitrary value: if it is itself a promise, chain to it */
static void js_resolve(Interp *I, JObj *p, JVal val){
    if(val.t==T_OBJ && val.u.obj && val.u.obj->htag==H_PROMISE){
        JObj *src=val.u.obj;
        if(js_prom_state(src)!=0) js_mq_push(I,0,0,p,js_prom_state(src),js_prom_value(src));
        else { JObj *rx=new_obj(I,K_OBJ); if(rx){ obj_set(I,rx,"p",jobj(p));
               JVal rxv; if(obj_get(src,"__rxs",&rxv)&&rxv.t==T_OBJ) arr_push(I,rxv.u.obj,jobj(rx)); } }
        return;
    }
    js_settle(I,p,1,val);
}
/* promise.then(onF,onR) -> a new promise */
static JObj *js_then(Interp *I, JObj *p, JVal onF, JVal onR){
    JObj *rp=js_new_promise(I); if(!rp)return 0;
    JObj *fF=(onF.t==T_OBJ&&(onF.u.obj->kind==K_FUN||onF.u.obj->kind==K_NATIVE))?onF.u.obj:0;
    JObj *fR=(onR.t==T_OBJ&&(onR.u.obj->kind==K_FUN||onR.u.obj->kind==K_NATIVE))?onR.u.obj:0;
    if(js_prom_state(p)!=0) js_mq_push(I,fF,fR,rp,js_prom_state(p),js_prom_value(p));
    else { JObj *rx=new_obj(I,K_OBJ); if(rx){ obj_set(I,rx,"f",fF?jobj(fF):jundef());
           obj_set(I,rx,"r",fR?jobj(fR):jundef()); obj_set(I,rx,"p",jobj(rp));
           JVal rxv; if(obj_get(p,"__rxs",&rxv)&&rxv.t==T_OBJ) arr_push(I,rxv.u.obj,jobj(rx)); } }
    return rp;
}
/* drain the microtask FIFO: run each pending reaction, chaining its result */
static void js_drain_micro(Interp *I){
    int guard=0;
    while(I->mq_count>0 && guard++<200000){
        int idx=I->mq_head; I->mq_head=(I->mq_head+1)%128; I->mq_count--;
        JObj *onF=I->microq[idx].onF, *onR=I->microq[idx].onR, *rp=I->microq[idx].rp;
        int state=I->microq[idx].state; JVal val=I->microq[idx].val;
        JObj *h = state==1?onF:onR;
        if(h){
            I->errflag=0; I->flow=0;
            JVal res=call_fn(I,h,jundef(),&val,1);
            if(I->errflag){ I->errflag=0; if(rp) js_settle(I,rp,2,jstr(arena_str(&I->a,I->err,strlen(I->err)))); }
            else if(rp) js_resolve(I,rp,res);
        } else if(rp) {
            js_settle(I,rp,state,val);                /* no handler: pass through */
        }
        if(I->a.oom) break;
    }
}

/* ---- minimal JSON.parse (integers only; objects/arrays/strings/bool/null) -- */
typedef struct { Interp *I; const char *s; int depth; } JParse;
static void jp_ws(JParse *j){ while(*j->s==' '||*j->s=='\t'||*j->s=='\n'||*j->s=='\r')j->s++; }
static JVal jp_value(JParse *j);
static JVal jp_string(JParse *j){
    char tmp[256]; int ti=0; j->s++;                  /* skip opening quote */
    while(*j->s && *j->s!='"'){
        char c=*j->s++;
        if(c=='\\' && *j->s){ char e=*j->s++;
            c = e=='n'?'\n':e=='t'?'\t':e=='r'?'\r':e=='b'?'\b':e=='f'?'\f':e;
            if(e=='u'){ int code=0; for(int k=0;k<4&&*j->s;k++){ char h=*j->s++; int d=(h>='0'&&h<='9')?h-'0':((h|32)>='a'&&(h|32)<='f')?(h|32)-'a'+10:0; code=code*16+d; } c=(char)(code&0x7f); }
        }
        if(ti<255) tmp[ti++]=c;
    }
    if(*j->s=='"') j->s++;
    tmp[ti]=0; return jstr(arena_str(&j->I->a,tmp,ti));
}
static JVal jp_value(JParse *j){
    jp_ws(j);
    if(j->depth>32 || !*j->s) return jnull();
    char c=*j->s;
    if(c=='"') return jp_string(j);
    if(c=='{'){ j->s++; j->depth++; JObj *o=new_obj(j->I,K_OBJ);
        jp_ws(j); if(*j->s=='}'){ j->s++; j->depth--; return o?jobj(o):jnull(); }
        for(;;){ jp_ws(j); if(*j->s!='"'){ break; } JVal k=jp_string(j); jp_ws(j);
            if(*j->s==':') j->s++; JVal v=jp_value(j); if(o)obj_set(j->I,o,k.u.str,v);
            jp_ws(j); if(*j->s==','){ j->s++; continue; } if(*j->s=='}'){ j->s++; break; } break; }
        j->depth--; return o?jobj(o):jnull(); }
    if(c=='['){ j->s++; j->depth++; JObj *a=new_obj(j->I,K_ARR);
        jp_ws(j); if(*j->s==']'){ j->s++; j->depth--; return a?jobj(a):jnull(); }
        for(;;){ JVal v=jp_value(j); if(a)arr_push(j->I,a,v);
            jp_ws(j); if(*j->s==','){ j->s++; continue; } if(*j->s==']'){ j->s++; break; } break; }
        j->depth--; return a?jobj(a):jnull(); }
    if(c=='t'){ j->s+=(j->s[1]=='r')?4:1; return jbool(1); }
    if(c=='f'){ j->s+=(j->s[1]=='a')?5:1; return jbool(0); }
    if(c=='n'){ j->s+=(j->s[1]=='u')?4:1; return jnull(); }
    /* number (integer part only; a fractional tail is consumed and dropped) */
    int neg=0; if(c=='-'){ neg=1; j->s++; } int64_t v=0; int any=0;
    while(*j->s>='0'&&*j->s<='9'){ v=v*10+(*j->s-'0'); j->s++; any=1; }
    if(*j->s=='.'){ j->s++; while(*j->s>='0'&&*j->s<='9')j->s++; }
    if(*j->s=='e'||*j->s=='E'){ j->s++; if(*j->s=='+'||*j->s=='-')j->s++; while(*j->s>='0'&&*j->s<='9')j->s++; }
    if(!any) return jnull();
    return jnum(neg?-v:v);
}
static JVal js_json_parse(Interp *I, const char *src){
    JParse j; j.I=I; j.s=src?src:""; j.depth=0; return jp_value(&j);
}

static int member_get(Interp *I, JVal recv, const char *name, JVal *out) {
    if (recv.t==T_STR) {
        if (strcmp(name,"length")==0){ *out=jnum(strlen(recv.u.str)); return 1; }
        return 0;                              /* methods via method_call */
    }
    if (recv.t==T_OBJ) {
        JObj *o=recv.u.obj;
        if (o->kind==K_ARR && strcmp(name,"length")==0){ *out=jnum(o->len); return 1; }
        if (o->dom && I->host) {
            if (strcmp(name,"innerHTML")==0||strcmp(name,"textContent")==0||strcmp(name,"innerText")==0) {
                char buf[8192]; buf[0]=0;
                I->host->get_inner(I->host->ud,o->dom,buf,sizeof buf);
                *out=jstr(arena_str(&I->a,buf,strlen(buf))); return 1;
            }
            if (I->host->get_attr) {                 /* el.id / className / href / src / value */
                const char *an = strcmp(name,"className")==0?"class":name;
                if (strcmp(name,"id")==0||strcmp(name,"className")==0||strcmp(name,"href")==0||
                    strcmp(name,"src")==0||strcmp(name,"value")==0||strcmp(name,"alt")==0||strcmp(name,"name")==0) {
                    char b[512]; b[0]=0; I->host->get_attr(I->host->ud,o->dom,an,b,sizeof b);
                    *out=jstr(arena_str(&I->a,b,strlen(b))); return 1;
                }
            }
        }
        JVal v; if (obj_get(o,name,&v)){ *out=v; return 1; }
        if (o->htag==H_DOCUMENT) {
            if (strcmp(name,"body")==0 || strcmp(name,"documentElement")==0) {
                *out = I->host? wrap_dom(I,I->host->get_by_tag(I->host->ud,"body",0)) : jnull(); return 1;
            }
            if (strcmp(name,"cookie")==0) {
                char b[1024]; b[0]=0; if(I->host&&I->host->cookie_get)I->host->cookie_get(I->host->ud,b,sizeof b);
                *out=jstr(arena_str(&I->a,b,strlen(b))); return 1;
            }
        }
        if (o->htag==H_WINDOW) {
            /* window.X resolves to a global of that name */
            Prop *p=env_find(I->global,name); if(p){ *out=p->val; return 1; }
        }
        return 0;
    }
    return 0;
}

static JVal method_call(Interp *I, JVal recv, const char *name, JVal *args, int nargs, int *handled) {
    *handled=1;
    JVal a0 = nargs>0?args[0]:jundef();
    JVal a1 = nargs>1?args[1]:jundef();

    if (recv.t==T_STR) {
        const char *s=recv.u.str; int L=strlen(s);
        if (strcmp(name,"charAt")==0){ int i=(int)to_num(a0); if(i<0||i>=L)return jstr(""); char c[2]={s[i],0}; return jstr(arena_str(&I->a,c,1)); }
        if (strcmp(name,"charCodeAt")==0){ int i=(int)to_num(a0); if(i<0||i>=L)return jnum(0); return jnum((unsigned char)s[i]); }
        if (strcmp(name,"indexOf")==0) return jnum(str_index_of(s,val_to_str(I,a0),nargs>1?(int)to_num(a1):0));
        if (strcmp(name,"includes")==0) return jbool(str_index_of(s,val_to_str(I,a0),0)>=0);
        if (strcmp(name,"startsWith")==0){ const char*n=val_to_str(I,a0); return jbool(strncmp(s,n,strlen(n))==0); }
        if (strcmp(name,"endsWith")==0){ const char*n=val_to_str(I,a0); int nl=strlen(n); return jbool(nl<=L && strcmp(s+L-nl,n)==0); }
        if (strcmp(name,"toUpperCase")==0){ char*r=arena_str(&I->a,s,L); for(int i=0;i<L;i++) if(r[i]>='a'&&r[i]<='z')r[i]-=32; return jstr(r); }
        if (strcmp(name,"toLowerCase")==0){ char*r=arena_str(&I->a,s,L); for(int i=0;i<L;i++) if(r[i]>='A'&&r[i]<='Z')r[i]+=32; return jstr(r); }
        if (strcmp(name,"trim")==0){ int b=0,e=L; while(b<e&&(s[b]==' '||s[b]=='\t'||s[b]=='\n'||s[b]=='\r'))b++; while(e>b&&(s[e-1]==' '||s[e-1]=='\t'||s[e-1]=='\n'||s[e-1]=='\r'))e--; return jstr(arena_str(&I->a,s+b,e-b)); }
        if (strcmp(name,"slice")==0||strcmp(name,"substring")==0){ int st=(int)to_num(a0); int en=nargs>1?(int)to_num(a1):L; if(st<0)st+=L; if(en<0)en+=L; if(st<0)st=0; if(en>L)en=L; if(en<st)en=st; return jstr(substr_dup(I,s,st,en-st)); }
        if (strcmp(name,"substr")==0){ int st=(int)to_num(a0); int ln=nargs>1?(int)to_num(a1):L-st; return jstr(substr_dup(I,s,st,ln)); }
        if (strcmp(name,"repeat")==0){ int k=(int)to_num(a0); char*r=""; for(int i=0;i<k;i++)r=str_concat(I,r,s); return jstr(r); }
        if (strcmp(name,"concat")==0){ char*r=arena_str(&I->a,s,L); for(int i=0;i<nargs;i++)r=str_concat(I,r,val_to_str(I,args[i])); return jstr(r); }
        if (strcmp(name,"replace")==0){ const char*from=val_to_str(I,a0),*to=val_to_str(I,a1); int idx=str_index_of(s,from,0); if(idx<0)return jstr(arena_str(&I->a,s,L)); char*r=substr_dup(I,s,0,idx); r=str_concat(I,r,to); r=str_concat(I,r,s+idx+strlen(from)); return jstr(r); }
        if (strcmp(name,"split")==0){ JObj*arr=new_obj(I,K_ARR); const char*sep=nargs>0?val_to_str(I,a0):0;
            if(!sep||sep[0]==0){ for(int i=0;i<L;i++){ char c[2]={s[i],0}; arr_push(I,arr,jstr(arena_str(&I->a,c,1))); } }
            else { int sl=strlen(sep),start=0; for(;;){ int idx=str_index_of(s,sep,start); if(idx<0){ arr_push(I,arr,jstr(arena_str(&I->a,s+start,L-start))); break; } arr_push(I,arr,jstr(substr_dup(I,s,start,idx-start))); start=idx+sl; } }
            return jobj(arr); }
        if (strcmp(name,"toString")==0) return recv;
        *handled=0; return jundef();
    }

    if (recv.t==T_NUM) {
        if (strcmp(name,"toString")==0) return jstr(i64_to_str(I,recv.u.num));
        if (strcmp(name,"toFixed")==0)  return jstr(i64_to_str(I,recv.u.num));
        *handled=0; return jundef();
    }

    if (recv.t==T_OBJ) {
        JObj *o=recv.u.obj;

        if (o->kind==K_ARR) {
            if (strcmp(name,"push")==0){ for(int i=0;i<nargs;i++)arr_push(I,o,args[i]); return jnum(o->len); }
            if (strcmp(name,"pop")==0){ if(o->len==0)return jundef(); return o->items[--o->len]; }
            if (strcmp(name,"shift")==0){ if(o->len==0)return jundef(); JVal v=o->items[0]; for(int i=1;i<o->len;i++)o->items[i-1]=o->items[i]; o->len--; return v; }
            if (strcmp(name,"unshift")==0){ for(int k=nargs-1;k>=0;k--){ arr_push(I,o,jundef()); for(int i=o->len-1;i>0;i--)o->items[i]=o->items[i-1]; o->items[0]=args[k]; } return jnum(o->len); }
            if (strcmp(name,"join")==0){ const char*sep=nargs>0?val_to_str(I,a0):","; char*r=""; for(int i=0;i<o->len;i++){ if(i)r=str_concat(I,r,sep); r=str_concat(I,r,val_to_str(I,o->items[i])); } return jstr(r); }
            if (strcmp(name,"indexOf")==0){ for(int i=0;i<o->len;i++) if(strict_eq(o->items[i],a0))return jnum(i); return jnum(-1); }
            if (strcmp(name,"includes")==0){ for(int i=0;i<o->len;i++) if(strict_eq(o->items[i],a0))return jbool(1); return jbool(0); }
            if (strcmp(name,"slice")==0){ int st=(int)to_num(a0); int en=nargs>1?(int)to_num(a1):o->len; if(st<0)st+=o->len; if(en<0)en+=o->len; if(st<0)st=0; if(en>o->len)en=o->len; JObj*r=new_obj(I,K_ARR); for(int i=st;i<en;i++)arr_push(I,r,o->items[i]); return jobj(r); }
            if (strcmp(name,"concat")==0){ JObj*r=new_obj(I,K_ARR); for(int i=0;i<o->len;i++)arr_push(I,r,o->items[i]); for(int k=0;k<nargs;k++){ if(args[k].t==T_OBJ&&args[k].u.obj->kind==K_ARR){ JObj*x=args[k].u.obj; for(int i=0;i<x->len;i++)arr_push(I,r,x->items[i]); } else arr_push(I,r,args[k]); } return jobj(r); }
            if (strcmp(name,"reverse")==0){ for(int i=0,j=o->len-1;i<j;i++,j--){ JVal t=o->items[i]; o->items[i]=o->items[j]; o->items[j]=t; } return recv; }
            if (strcmp(name,"forEach")==0 && a0.t==T_OBJ){ for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; call_fn(I,a0.u.obj,jundef(),ar,2); if(I->errflag)break; } return jundef(); }
            if (strcmp(name,"map")==0 && a0.t==T_OBJ){ JObj*r=new_obj(I,K_ARR); for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; arr_push(I,r,call_fn(I,a0.u.obj,jundef(),ar,2)); if(I->errflag)break; } return jobj(r); }
            if (strcmp(name,"filter")==0 && a0.t==T_OBJ){ JObj*r=new_obj(I,K_ARR); for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; if(truthy(call_fn(I,a0.u.obj,jundef(),ar,2)))arr_push(I,r,o->items[i]); if(I->errflag)break; } return jobj(r); }
            if (strcmp(name,"find")==0 && a0.t==T_OBJ){ for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; if(truthy(call_fn(I,a0.u.obj,jundef(),ar,2)))return o->items[i]; } return jundef(); }
            if (strcmp(name,"some")==0 && a0.t==T_OBJ){ for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; if(truthy(call_fn(I,a0.u.obj,jundef(),ar,2)))return jbool(1); } return jbool(0); }
            if (strcmp(name,"every")==0 && a0.t==T_OBJ){ for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; if(!truthy(call_fn(I,a0.u.obj,jundef(),ar,2)))return jbool(0); } return jbool(1); }
            if (strcmp(name,"reduce")==0 && a0.t==T_OBJ){ JVal acc; int start=0; if(nargs>1){acc=a1;}else{ if(o->len==0)return jundef(); acc=o->items[0]; start=1; } for(int i=start;i<o->len;i++){ JVal ar[3]={acc,o->items[i],jnum(i)}; acc=call_fn(I,a0.u.obj,jundef(),ar,3); if(I->errflag)break; } return acc; }
            if (strcmp(name,"toString")==0||strcmp(name,"join")==0) return jstr(val_to_str(I,recv));
            *handled=0; return jundef();
        }

        if (o->htag==H_CONSOLE) {
            if (strcmp(name,"log")==0||strcmp(name,"error")==0||strcmp(name,"warn")==0||strcmp(name,"info")==0||strcmp(name,"debug")==0) {
                if (I->host) I->host->log(I->host->ud, join_args(I,args,nargs," "));
                return jundef();
            }
            *handled=0; return jundef();
        }
        if (o->htag==H_MATH) {
            int64_t x=to_num(a0);
            if (strcmp(name,"floor")==0||strcmp(name,"ceil")==0||strcmp(name,"round")==0||strcmp(name,"trunc")==0) return jnum(x);
            if (strcmp(name,"abs")==0) return jnum(x<0?-x:x);
            if (strcmp(name,"sign")==0) return jnum(x>0?1:x<0?-1:0);
            if (strcmp(name,"sqrt")==0){ if(x<0)return jnum(0); int64_t r=0; while((r+1)*(r+1)<=x)r++; return jnum(r); }
            if (strcmp(name,"pow")==0){ int64_t b=x,e=to_num(a1),r=1; for(int64_t i=0;i<e&&i<64;i++)r*=b; return jnum(r); }
            if (strcmp(name,"max")==0){ int64_t m=nargs?to_num(args[0]):0; for(int i=1;i<nargs;i++){ int64_t v=to_num(args[i]); if(v>m)m=v; } return jnum(m); }
            if (strcmp(name,"min")==0){ int64_t m=nargs?to_num(args[0]):0; for(int i=1;i<nargs;i++){ int64_t v=to_num(args[i]); if(v<m)m=v; } return jnum(m); }
            if (strcmp(name,"random")==0) return jnum(0);     /* no float: best we can do */
            *handled=0; return jundef();
        }
        if (o->htag==H_DOCUMENT) {
            if (!I->host){ return jnull(); }
            if (strcmp(name,"getElementById")==0) return wrap_dom(I,I->host->get_by_id(I->host->ud,val_to_str(I,a0)));
            if (strcmp(name,"getElementsByTagName")==0||strcmp(name,"querySelectorAll")==0){ JObj*r=new_obj(I,K_ARR); for(int i=0;i<64;i++){ js_dom_node d=I->host->get_by_tag(I->host->ud,val_to_str(I,a0),i); if(!d)break; arr_push(I,r,wrap_dom(I,d)); } return jobj(r); }
            if (strcmp(name,"querySelector")==0){ const char*sel=val_to_str(I,a0);
                if (I->host->query) return wrap_dom(I,I->host->query(I->host->ud,sel));
                if (sel[0]=='#') return wrap_dom(I,I->host->get_by_id(I->host->ud,sel+1));
                return wrap_dom(I,I->host->get_by_tag(I->host->ud,sel,0)); }
            if (strcmp(name,"write")==0||strcmp(name,"writeln")==0){ I->host->doc_write(I->host->ud,join_args(I,args,nargs,"")); if(strcmp(name,"writeln")==0)I->host->doc_write(I->host->ud,"\n"); return jundef(); }
            if (strcmp(name,"createElement")==0){ return I->host->create_el? wrap_dom(I,I->host->create_el(I->host->ud,val_to_str(I,a0))) : wrap_dom(I,0); }
            if (strcmp(name,"addEventListener")==0){ js_add_listener(I, JS_TARGET_DOC, val_to_str(I,a0), a1); return jundef(); }
            *handled=0; return jundef();
        }
        if (o->htag==H_WINDOW) {
            if (strcmp(name,"addEventListener")==0){ js_add_listener(I, JS_TARGET_DOC, val_to_str(I,a0), a1); return jundef(); }
            if (strcmp(name,"setTimeout")==0||strcmp(name,"setInterval")==0||strcmp(name,"requestAnimationFrame")==0||strcmp(name,"alert")==0){ *handled=0; return jundef(); }
        }
        if (o->htag==H_LOCALSTORAGE) {
            if (!I->host){ return jnull(); }
            if (strcmp(name,"getItem")==0){ char b[1024]; b[0]=0; int ok = I->host->ls_get? I->host->ls_get(I->host->ud,val_to_str(I,a0),b,sizeof b):0; return ok? jstr(arena_str(&I->a,b,strlen(b))) : jnull(); }
            if (strcmp(name,"setItem")==0){ if(I->host->ls_set)I->host->ls_set(I->host->ud,val_to_str(I,a0),val_to_str(I,a1)); return jundef(); }
            if (strcmp(name,"removeItem")==0){ if(I->host->ls_remove)I->host->ls_remove(I->host->ud,val_to_str(I,a0)); return jundef(); }
            *handled=0; return jundef();
        }
        if (o->htag==H_JSON) {
            if (strcmp(name,"stringify")==0) { /* minimal: numbers/strings/bools/arrays */
                return jstr(val_to_str(I,a0)); }
            if (strcmp(name,"parse")==0) { return js_json_parse(I, val_to_str(I,a0)); }
            *handled=0; return jundef();
        }
        if (o->htag==H_PROMISE) {
            if (strcmp(name,"then")==0)    return jobj(js_then(I,o,a0,a1));
            if (strcmp(name,"catch")==0)   return jobj(js_then(I,o,jundef(),a0));
            if (strcmp(name,"finally")==0) return jobj(js_then(I,o,a0,a0));
            *handled=0; return jundef();
        }
        if (o->htag==H_RESPONSE) {
            JVal bodyv=jundef(); obj_get(o,"__body",&bodyv);
            const char *body = bodyv.t==T_STR?bodyv.u.str:"";
            if (strcmp(name,"text")==0){ JObj*pr=js_new_promise(I); js_settle(I,pr,1,jstr(arena_str(&I->a,body,strlen(body)))); return jobj(pr); }
            if (strcmp(name,"json")==0){ JObj*pr=js_new_promise(I); js_settle(I,pr,1,js_json_parse(I,body)); return jobj(pr); }
            *handled=0; return jundef();
        }
        if (o->dom && I->host) {
            if (strcmp(name,"setAttribute")==0){ if(I->host->set_attr)I->host->set_attr(I->host->ud,o->dom,val_to_str(I,a0),val_to_str(I,a1)); return jundef(); }
            if (strcmp(name,"getAttribute")==0){ if(I->host->get_attr){ char b[512]; b[0]=0; if(I->host->get_attr(I->host->ud,o->dom,val_to_str(I,a0),b,sizeof b)) return jstr(arena_str(&I->a,b,strlen(b))); } return jnull(); }
            if (strcmp(name,"appendChild")==0){ if(I->host->append_child && a0.t==T_OBJ && a0.u.obj->dom) I->host->append_child(I->host->ud,o->dom,a0.u.obj->dom); return a0; }
            if (strcmp(name,"querySelector")==0){ if(I->host->query) return wrap_dom(I,I->host->query(I->host->ud,val_to_str(I,a0))); return jnull(); }
            if (strcmp(name,"addEventListener")==0){ js_add_listener(I, o->dom, val_to_str(I,a0), a1); return jundef(); }
            if (strcmp(name,"removeAttribute")==0||strcmp(name,"focus")==0||strcmp(name,"click")==0) return jundef();
            *handled=0; return jundef();
        }

        /* user function stored as a property: let caller handle it */
        *handled=0; return jundef();
    }

    *handled=0; return jundef();
}

/* ----------------------------- global natives -------------------------- */
static JVal nf_parseInt(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; if(n==0)return jnum(0); return jnum(to_num(a[0])); }
static JVal nf_parseFloat(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; if(n==0)return jnum(0); return jnum(to_num(a[0])); }
static JVal nf_isNaN(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; (void)a;(void)n; return jbool(0); }
static JVal nf_String(Interp *I, JVal t, JVal *a, int n){ (void)t; if(n==0)return jstr(""); return jstr(val_to_str(I,a[0])); }
static JVal nf_Number(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; if(n==0)return jnum(0); return jnum(to_num(a[0])); }
static JVal nf_Boolean(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; if(n==0)return jbool(0); return jbool(truthy(a[0])); }
static JVal nf_Array(Interp *I, JVal t, JVal *a, int n){ (void)t; JObj*r=new_obj(I,K_ARR); if(n==1&&a[0].t==T_NUM){ int k=(int)a[0].u.num; for(int i=0;i<k;i++)arr_push(I,r,jundef()); } else for(int i=0;i<n;i++)arr_push(I,r,a[i]); return jobj(r); }
static JVal nf_alert(Interp *I, JVal t, JVal *a, int n){ (void)t; if(I->host)I->host->log(I->host->ud,join_args(I,a,n," ")); return jundef(); }
static JVal nf_noop(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t;(void)a;(void)n; return jundef(); }
static JVal nf_setTimeout(Interp *I, JVal t, JVal *a, int n){ (void)t;
    if(n<1||a[0].t!=T_OBJ||(a[0].u.obj->kind!=K_FUN&&a[0].u.obj->kind!=K_NATIVE))return jnum(-1);
    if(I->ntimers>=32)return jnum(-1);
    uint32_t ms=n>1?(uint32_t)to_num(a[1]):0;
    int id=I->ntimers++; I->timers[id].fn=a[0].u.obj; I->timers[id].due=I->now_ms+ms; I->timers[id].interval=0; I->timers[id].active=1;
    return jnum(id);
}
static JVal nf_setInterval(Interp *I, JVal t, JVal *a, int n){ (void)t;
    if(n<1||a[0].t!=T_OBJ||(a[0].u.obj->kind!=K_FUN&&a[0].u.obj->kind!=K_NATIVE))return jnum(-1);
    if(I->ntimers>=32)return jnum(-1);
    uint32_t ms=n>1?(uint32_t)to_num(a[1]):0; if(ms<16)ms=16;
    int id=I->ntimers++; I->timers[id].fn=a[0].u.obj; I->timers[id].due=I->now_ms+ms; I->timers[id].interval=ms; I->timers[id].active=1;
    return jnum(id);
}
static JVal nf_clearTimer(Interp *I, JVal t, JVal *a, int n){ (void)t; if(n>=1){ int id=(int)to_num(a[0]); if(id>=0&&id<I->ntimers)I->timers[id].active=0; } return jundef(); }
static JVal nf_addEventListener(Interp *I, JVal t, JVal *a, int n){ (void)t; if(n>=2)js_add_listener(I,JS_TARGET_DOC,val_to_str(I,a[0]),a[1]); return jundef(); }

/* ---- fetch + Promise constructor/statics ---- */
#define FETCH_CAP (48*1024)
static JVal nf_fetch(Interp *I, JVal t, JVal *a, int n){ (void)t;
    JObj *p=js_new_promise(I); if(!p)return jundef();
    if(n<1 || !I->host || !I->host->fetch){ js_settle(I,p,2,jstr("fetch: unavailable")); return jobj(p); }
    const char *url=val_to_str(I,a[0]);
    char *buf=(char*)arena_alloc(&I->a,FETCH_CAP); if(!buf){ js_settle(I,p,2,jstr("fetch: oom")); return jobj(p); }
    buf[0]=0; int status=0;
    int len=I->host->fetch(I->host->ud,url,buf,FETCH_CAP,&status);
    if(len<0){ js_settle(I,p,2,jstr("fetch: network error")); return jobj(p); }
    if(len>=FETCH_CAP) len=FETCH_CAP-1; buf[len]=0;
    JObj *resp=new_obj(I,K_OBJ); if(!resp){ js_settle(I,p,2,jstr("fetch: oom")); return jobj(p); }
    resp->htag=H_RESPONSE;
    obj_set(I,resp,"status",jnum(status));
    obj_set(I,resp,"ok",jbool(status>=200&&status<300));
    obj_set(I,resp,"url",jstr(arena_str(&I->a,url,strlen(url))));
    obj_set(I,resp,"__body",jstr(buf));
    js_settle(I,p,1,jobj(resp));
    return jobj(p);
}
/* make a native fn that, when called, receives `bind` as its `this` (call_fn
 * passes bound_this whenever dom is non-null) */
static JObj *make_bound_native(Interp *I, NativeFn fn, JObj *bind){
    JObj *o=new_obj(I,K_NATIVE); if(!o)return 0;
    o->nat=fn; o->dom=(js_dom_node)bind; o->bound_this=jobj(bind); return o;
}
static JVal nf_res_bound(Interp *I, JVal thisv, JVal *a, int n){ if(thisv.t==T_OBJ) js_resolve(I,thisv.u.obj,n?a[0]:jundef()); return jundef(); }
static JVal nf_rej_bound(Interp *I, JVal thisv, JVal *a, int n){ if(thisv.t==T_OBJ) js_settle (I,thisv.u.obj,2,n?a[0]:jundef()); return jundef(); }
static JVal nf_Promise(Interp *I, JVal thisv, JVal *a, int n){ (void)thisv;
    JObj *p=js_new_promise(I); if(!p)return jundef();
    if(n>=1 && a[0].t==T_OBJ && (a[0].u.obj->kind==K_FUN||a[0].u.obj->kind==K_NATIVE)){
        JObj *res=make_bound_native(I,nf_res_bound,p), *rej=make_bound_native(I,nf_rej_bound,p);
        JVal args[2]={ res?jobj(res):jundef(), rej?jobj(rej):jundef() };
        I->errflag=0; call_fn(I,a[0].u.obj,jundef(),args,2);
        if(I->errflag){ I->errflag=0; js_settle(I,p,2,jstr(arena_str(&I->a,I->err,strlen(I->err)))); }
    }
    return jobj(p);
}
static JVal nf_Promise_resolve(Interp *I, JVal t, JVal *a, int n){ (void)t; JObj*p=js_new_promise(I); js_resolve(I,p,n?a[0]:jundef()); return jobj(p); }
static JVal nf_Promise_reject (Interp *I, JVal t, JVal *a, int n){ (void)t; JObj*p=js_new_promise(I); js_settle (I,p,2,n?a[0]:jundef()); return jobj(p); }
static JVal nf_all_elem(Interp *I, JVal thisv, JVal *a, int n){       /* this=binder{c,i} */
    if(thisv.t!=T_OBJ)return jundef(); JObj*b=thisv.u.obj;
    JVal cv=jundef(),iv=jundef(); obj_get(b,"c",&cv); obj_get(b,"i",&iv);
    if(cv.t!=T_OBJ)return jundef(); JObj*ctrl=cv.u.obj; int idx=(int)iv.u.num;
    JVal rv=jundef(),remv=jundef(),pv=jundef(); obj_get(ctrl,"r",&rv); obj_get(ctrl,"n",&remv); obj_get(ctrl,"p",&pv);
    if(rv.t==T_OBJ && idx>=0 && idx<rv.u.obj->len) rv.u.obj->items[idx]= n?a[0]:jundef();
    int rem=(int)remv.u.num-1; obj_set(I,ctrl,"n",jnum(rem));
    if(rem<=0 && pv.t==T_OBJ) js_settle(I,pv.u.obj,1,rv);
    return jundef();
}
static JVal nf_all_rej(Interp *I, JVal thisv, JVal *a, int n){        /* this=ctrl */
    if(thisv.t!=T_OBJ)return jundef(); JVal pv=jundef(); obj_get(thisv.u.obj,"p",&pv);
    if(pv.t==T_OBJ) js_settle(I,pv.u.obj,2,n?a[0]:jundef()); return jundef();
}
static JVal nf_Promise_all(Interp *I, JVal t, JVal *a, int n){ (void)t;
    JObj *p=js_new_promise(I); if(!p)return jundef();
    if(n<1 || a[0].t!=T_OBJ || a[0].u.obj->kind!=K_ARR){ js_settle(I,p,1,n?a[0]:jundef()); return jobj(p); }
    JObj *arr=a[0].u.obj;
    JObj *results=new_obj(I,K_ARR); for(int i=0;i<arr->len;i++)arr_push(I,results,jundef());
    JObj *ctrl=new_obj(I,K_OBJ); obj_set(I,ctrl,"r",jobj(results)); obj_set(I,ctrl,"n",jnum(arr->len)); obj_set(I,ctrl,"p",jobj(p));
    if(arr->len==0){ js_settle(I,p,1,jobj(results)); return jobj(p); }
    JObj *rej=make_bound_native(I,nf_all_rej,ctrl);
    for(int i=0;i<arr->len;i++){
        JObj *binder=new_obj(I,K_OBJ); obj_set(I,binder,"c",jobj(ctrl)); obj_set(I,binder,"i",jnum(i));
        JObj *elem=make_bound_native(I,nf_all_elem,binder);
        JVal item=arr->items[i];
        if(item.t==T_OBJ && item.u.obj->htag==H_PROMISE) js_then(I,item.u.obj, elem?jobj(elem):jundef(), rej?jobj(rej):jundef());
        else if(elem){ JVal one=item; call_fn(I,elem,jundef(),&one,1); }
    }
    return jobj(p);
}

static void def_native(Interp *I, const char *name, NativeFn fn) {
    JObj *o=new_obj(I,K_NATIVE); if(!o)return; o->nat=fn; env_def(I,I->global,name,jobj(o));
}
static JObj *host_obj(Interp *I, int htag) { JObj *o=new_obj(I,K_OBJ); if(o)o->htag=htag; return o; }

static void js_install_globals(Interp *I) {
    env_def(I,I->global,"console", jobj(host_obj(I,H_CONSOLE)));
    env_def(I,I->global,"Math",    jobj(host_obj(I,H_MATH)));
    env_def(I,I->global,"document",jobj(host_obj(I,H_DOCUMENT)));
    env_def(I,I->global,"JSON",    jobj(host_obj(I,H_JSON)));
    JObj *win=host_obj(I,H_WINDOW);
    env_def(I,I->global,"window",  jobj(win));
    env_def(I,I->global,"globalThis", jobj(win));
    env_def(I,I->global,"localStorage", jobj(host_obj(I,H_LOCALSTORAGE)));
    env_def(I,I->global,"sessionStorage", jobj(host_obj(I,H_LOCALSTORAGE)));
    env_def(I,I->global,"undefined", jundef());
    env_def(I,I->global,"NaN", jnum(0));
    env_def(I,I->global,"Infinity", jnum(0x7fffffffffffffffLL));
    def_native(I,"parseInt",nf_parseInt);
    def_native(I,"parseFloat",nf_parseFloat);
    def_native(I,"isNaN",nf_isNaN);
    def_native(I,"String",nf_String);
    def_native(I,"Number",nf_Number);
    def_native(I,"Boolean",nf_Boolean);
    def_native(I,"Array",nf_Array);
    def_native(I,"alert",nf_alert);
    def_native(I,"setTimeout",nf_setTimeout);
    def_native(I,"setInterval",nf_setInterval);
    def_native(I,"requestAnimationFrame",nf_setTimeout);
    def_native(I,"clearTimeout",nf_clearTimer);
    def_native(I,"clearInterval",nf_clearTimer);
    def_native(I,"addEventListener",nf_addEventListener);
    def_native(I,"fetch",nf_fetch);
    /* Promise: callable as `new Promise(fn)` plus static resolve/reject/all */
    { JObj *P=new_obj(I,K_NATIVE);
      if(P){ P->nat=nf_Promise;
        JObj *r=new_obj(I,K_NATIVE);  if(r){  r->nat=nf_Promise_resolve; obj_set(I,P,"resolve",jobj(r)); }
        JObj *j=new_obj(I,K_NATIVE);  if(j){  j->nat=nf_Promise_reject;  obj_set(I,P,"reject", jobj(j)); }
        JObj *al=new_obj(I,K_NATIVE); if(al){ al->nat=nf_Promise_all;    obj_set(I,P,"all",    jobj(al)); }
        env_def(I,I->global,"Promise",jobj(P)); } }
}
