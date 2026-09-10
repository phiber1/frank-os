/*
 * asm_thumb.c — portable Thumb-16 assembler core for FRANK OS.
 * See asm_thumb.h.  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "asm_thumb.h"

/*==========================================================================
 * Freestanding helpers (no libc dependency beyond memcpy/memset)
 *=========================================================================*/
static int a_isspace(int c){ return c==' '||c=='\t'||c=='\r'||c=='\f'||c=='\v'; }
static int a_isdigit(int c){ return c>='0'&&c<='9'; }
static int a_isalpha(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static int a_islower(int c){ return c>='a'&&c<='z'; }
static int a_toupper(int c){ return a_islower(c)?c-32:c; }
static int a_isid0(int c){ return a_isalpha(c)||c=='_'||c=='.'||c=='$'; }
static int a_isid(int c){ return a_isid0(c)||a_isdigit(c); }

static int a_strlen(const char*s){ int n=0; while(s[n]) n++; return n; }
static int a_streq(const char*a,const char*b){ while(*a&&*b){ if(*a!=*b)return 0; a++;b++; } return *a==*b; }
/* case-insensitive compare */
static int a_ieq(const char*a,const char*b){
    while(*a&&*b){ if(a_toupper((unsigned char)*a)!=a_toupper((unsigned char)*b))return 0; a++;b++; }
    return *a==*b;
}
static void a_cpy(char*d,const char*s,int cap){ int i=0; while(s[i]&&i<cap-1){ d[i]=s[i]; i++; } d[i]=0; }

/*==========================================================================
 * Diagnostics
 *=========================================================================*/
static void err(asm_ctx*c,const char*msg){
    if(c->nerr>=ASM_MAX_ERRORS) return;
    c->err[c->nerr].line = c->cur_line;
    a_cpy(c->err[c->nerr].msg, msg, (int)sizeof c->err[0].msg);
    c->nerr++;
}

/*==========================================================================
 * Symbol table
 *=========================================================================*/
static asm_symbol* sym_find(asm_ctx*c,const char*name){
    for(int i=0;i<c->nsym;i++) if(a_streq(c->sym[i].name,name)) return &c->sym[i];
    return 0;
}
static asm_symbol* sym_add(asm_ctx*c,const char*name){
    asm_symbol*s=sym_find(c,name);
    if(s) return s;
    if(c->nsym>=ASM_MAX_SYMBOLS){ err(c,"symbol table full"); return 0; }
    s=&c->sym[c->nsym++];
    a_cpy(s->name,name,sizeof s->name);
    s->value=0; s->defined=0; s->is_equ=0;
    return s;
}

/*==========================================================================
 * Expression evaluator — labels, decimal/hex/char constants, . (PC),
 * unary +/-, binary + - * / & | << >> (left-to-right, no precedence:
 * adequate for assembler operands; use parens-free simple forms).
 *=========================================================================*/
static const char* skipws(const char*p){ while(*p&&a_isspace((unsigned char)*p)) p++; return p; }

static int eval_expr(asm_ctx*c,const char**pp,int min_prec,long*out);

/* parse one primary term into *out; advance *pp. returns 1 ok, 0 error. */
static int prim(asm_ctx*c,const char**pp,long*out){
    const char*p=skipws(*pp);
    if(*p=='('){
        const char*q=p+1; long v;
        if(!eval_expr(c,&q,1,&v)) return 0; q=skipws(q);
        if(*q!=')'){ err(c,"expected )"); return 0; }
        *out=v; *pp=q+1; return 1;
    }
    if(*p=='-'){ p++; long v; if(!prim(c,&p,&v))return 0; *out=-v; *pp=p; return 1; }
    if(*p=='+'){ p++; return prim(c,&p,out)?(*pp=p,1):0; }
    if(*p=='~'){ p++; long v; if(!prim(c,&p,&v))return 0; *out=~v; *pp=p; return 1; }
    if(*p=='\''){                      /* char literal 'a' */
        p++; int ch=*p++;
        if(*p!='\''){ err(c,"bad char literal"); return 0; }
        p++; *out=ch; *pp=p; return 1;
    }
    if(*p=='.'&&!a_isid((unsigned char)p[1])){   /* current address */
        *out=(long)c->pc; *pp=p+1; return 1;
    }
    if(a_isdigit((unsigned char)*p)){
        long v=0;
        if(p[0]=='0'&&(p[1]=='x'||p[1]=='X')){
            p+=2; while(1){ int d; char ch=*p;
                if(a_isdigit((unsigned char)ch))d=ch-'0';
                else if(ch>='a'&&ch<='f')d=ch-'a'+10;
                else if(ch>='A'&&ch<='F')d=ch-'A'+10;
                else break;
                v=v*16+d; p++; }
        } else {
            while(a_isdigit((unsigned char)*p)){ v=v*10+(*p-'0'); p++; }
        }
        *out=v; *pp=p; return 1;
    }
    if(a_isid0((unsigned char)*p)){
        char nm[ASM_MAX_LABELLEN+1]; int n=0;
        while(a_isid((unsigned char)*p)&&n<ASM_MAX_LABELLEN) nm[n++]=*p++;
        nm[n]=0;
        asm_symbol*s=sym_find(c,nm);
        if(!s||!s->defined){
            if(c->pass==2){ err(c,"undefined symbol"); return 0; }
            *out=0; *pp=p; return 1;         /* pass 1: unknown yet */
        }
        if(!s->is_equ) c->expr_used_sym=1;   /* address ref → needs reloc */
        *out=(long)s->value; *pp=p; return 1;
    }
    err(c,"bad expression"); return 0;
}

/* binary-operator precedence (higher binds tighter); -1 if not an op.
 * Matches GNU as (our validation reference), NOT C: highest = * / % << >>,
 * middle = & | ^, lowest = + -.  sets *oplen to the operator length. */
static int binop_prec(const char*p,int*oplen){
    if((p[0]=='<'&&p[1]=='<')||(p[0]=='>'&&p[1]=='>')){ *oplen=2; return 3; }
    *oplen=1;
    switch(*p){
        case '*': case '/': case '%': return 3;
        case '&': case '|': case '^': return 2;
        case '+': case '-':           return 1;
    }
    return -1;
}
static long binop_apply(char op,int oplen,long a,long b){
    if(oplen==2) return op=='<' ? (a<<b) : (a>>b);
    switch(op){
        case '*': return a*b;   case '/': return b? a/b:0;
        case '%': return b? a%b:0;
        case '+': return a+b;   case '-': return a-b;
        case '&': return a&b;   case '^': return a^b;  case '|': return a|b;
    }
    return a;
}

/* precedence-climbing expression parser (left-associative).  Parses
 * operators whose precedence is >= min_prec, then stops. */
static int eval_expr(asm_ctx*c,const char**pp,int min_prec,long*out){
    long lhs;
    if(!prim(c,pp,&lhs)) return 0;
    for(;;){
        const char*p=skipws(*pp);
        int oplen, prec=binop_prec(p,&oplen);
        if(prec<min_prec) break;
        char op=*p; const char*q=p+oplen;
        long rhs;
        if(!eval_expr(c,&q,prec+1,&rhs)) return 0;
        lhs=binop_apply(op,oplen,lhs,rhs);
        *pp=q;
    }
    *out=lhs; return 1;
}

/* full expression with C-like operator precedence */
static int eval(asm_ctx*c,const char*s,long*out){
    const char*p=s;
    c->expr_used_sym=0;
    return eval_expr(c,&p,1,out);
}

/*==========================================================================
 * Register + reglist parsing
 *=========================================================================*/
/* returns 0-15, or -1.  Accepts r0-r15, sp, lr, pc, fp/ip aliases. */
static int reg(const char*s){
    char b[8]; int n=0; const char*p=skipws(s);
    while(a_isid((unsigned char)*p)&&n<7) b[n++]=(char)a_toupper((unsigned char)*p++);
    b[n]=0;
    if(b[0]=='R'&&a_isdigit((unsigned char)b[1])){
        int v=b[1]-'0'; if(b[2]){ if(!a_isdigit((unsigned char)b[2])||b[3])return -1; v=v*10+(b[2]-'0'); }
        return (v>=0&&v<=15)?v:-1;
    }
    if(a_streq(b,"SP")) return 13;
    if(a_streq(b,"LR")) return 14;
    if(a_streq(b,"PC")) return 15;
    if(a_streq(b,"FP")) return 7;
    if(a_streq(b,"IP")) return 12;
    return -1;
}

/* parse "{r0,r4-r7,lr}" → bitmask (bits 0..15). returns 1 ok. */
static int reglist(asm_ctx*c,const char*s,int*mask){
    const char*p=skipws(s); *mask=0;
    if(*p!='{'){ err(c,"expected {"); return 0; }
    p++;
    for(;;){
        p=skipws(p);
        if(*p=='}'){ p++; break; }
        /* read a register token */
        char tok[8]; int n=0;
        while(a_isid((unsigned char)*p)&&n<7) tok[n++]=*p++;
        tok[n]=0;
        int r0=reg(tok); if(r0<0){ err(c,"bad register in list"); return 0; }
        int r1=r0;
        p=skipws(p);
        if(*p=='-'){
            p++; p=skipws(p); n=0; char t2[8];
            while(a_isid((unsigned char)*p)&&n<7) t2[n++]=*p++;
            t2[n]=0; r1=reg(t2); if(r1<0){ err(c,"bad range"); return 0; }
        }
        if(r1<r0){ err(c,"bad register range"); return 0; }
        for(int r=r0;r<=r1;r++) *mask|=(1<<r);
        p=skipws(p);
        if(*p==','){ p++; continue; }
    }
    return 1;
}

/*==========================================================================
 * Emit helpers
 *=========================================================================*/
static void emit16(asm_ctx*c,unsigned hw){
    if(c->pass==2){
        if(c->code_len+2>ASM_MAX_CODE){ err(c,"code overflow"); return; }
        c->code[c->code_len]   =(uint8_t)(hw&0xFF);
        c->code[c->code_len+1] =(uint8_t)((hw>>8)&0xFF);
    }
    c->code_len+=2; c->pc+=2;
}
static void emit8(asm_ctx*c,unsigned b){
    if(c->pass==2){
        if(c->code_len+1>ASM_MAX_CODE){ err(c,"code overflow"); return; }
        c->code[c->code_len]=(uint8_t)(b&0xFF);
    }
    c->code_len++; c->pc++;
}

/* split operands by commas at top level (not inside {} or ()).  Fills
 * argv[] with pointers into a mutable copy `buf`.  Returns count. */
static int split_ops(char*buf,char*argv[],int maxargs){
    int n=0,depth=0; char*start=buf; char*p=buf;
    /* skip leading ws */
    while(*start&&a_isspace((unsigned char)*start)) start++;
    p=start;
    if(!*p) return 0;
    argv[n++]=start;
    for(;*p;p++){
        if(*p=='{'||*p=='('||*p=='[') depth++;
        else if(*p=='}'||*p==')'||*p==']') depth--;
        else if(*p==','&&depth==0){
            *p=0;
            char*q=p+1; while(*q&&a_isspace((unsigned char)*q)) q++;
            if(n<maxargs) argv[n++]=q;
        }
    }
    /* right-trim each */
    for(int i=0;i<n;i++){
        char*e=argv[i]+a_strlen(argv[i]);
        while(e>argv[i]&&a_isspace((unsigned char)e[-1])) *--e=0;
    }
    return n;
}

/*==========================================================================
 * Instruction encoders.  Each returns 1 if it handled the mnemonic.
 * `ac`/`av` are the operand count/vector.  On encoding error it records
 * a diagnostic and still returns 1 (mnemonic was recognized).
 *=========================================================================*/
static int need(asm_ctx*c,int have,int want){
    if(have!=want){ err(c,"wrong operand count"); return 0; } return 1;
}
static int imm(asm_ctx*c,const char*s,long*v){ return eval(c,s,v); }

/* condition suffixes for B<cond> */
static int cond_code(const char*s){
    static const char*n[]={"EQ","NE","CS","CC","MI","PL","VS","VC",
                           "HI","LS","GE","LT","GT","LE","AL"};
    static const char*alt[]={0,0,"HS","LO",0,0,0,0,0,0,0,0,0,0,0};
    for(int i=0;i<15;i++){ if(a_ieq(s,n[i]))return i; if(alt[i]&&a_ieq(s,alt[i]))return i; }
    return -1;
}

/* Encode one instruction. mnem is uppercased. */
static void encode(asm_ctx*c,const char*mnem,char*ops){
    char*av[8]; char obuf[160];
    a_cpy(obuf,ops,sizeof obuf);
    int ac=split_ops(obuf,av,8);
    long v;

    /* --- no-operand --- */
    if(a_ieq(mnem,"NOP")){ emit16(c,0xBF00); return; }
    if(a_ieq(mnem,"BX")){ if(!need(c,ac,1))return; int m=reg(av[0]); if(m<0){err(c,"bad reg");return;} emit16(c,0x4700|(m<<3)); return; }
    if(a_ieq(mnem,"BLX")){ if(!need(c,ac,1))return; int m=reg(av[0]); if(m<0){err(c,"bad reg");return;} emit16(c,0x4780|(m<<3)); return; }
    if(a_ieq(mnem,"BKPT")){ long i=0; if(ac>=1){const char*a=av[0];if(*a=='#')a++;imm(c,a,&i);} emit16(c,0xBE00|((int)i&0xFF)); return; }
    if(a_ieq(mnem,"SVC")){ if(!need(c,ac,1))return; const char*a=av[0];if(*a=='#')a++; imm(c,a,&v); emit16(c,0xDF00|((int)v&0xFF)); return; }

    /* --- PUSH/POP --- */
    if(a_ieq(mnem,"PUSH")||a_ieq(mnem,"POP")){
        if(!need(c,ac,1))return; int mask; if(!reglist(c,av[0],&mask))return;
        int push=a_ieq(mnem,"PUSH");
        int extra=0;
        if(push){ if(mask&(1<<14)){extra=1;mask&=~(1<<14);} }  /* LR */
        else    { if(mask&(1<<15)){extra=1;mask&=~(1<<15);} }  /* PC */
        if(mask&~0xFF){ err(c,"push/pop only r0-r7 (+lr/pc)"); return; }
        emit16(c,(push?0xB400:0xBC00)|(extra<<8)|(mask&0xFF));
        return;
    }
    /* --- STMIA/LDMIA Rn!, {list} --- */
    if(a_ieq(mnem,"STMIA")||a_ieq(mnem,"LDMIA")||a_ieq(mnem,"STM")||a_ieq(mnem,"LDM")){
        if(!need(c,ac,2))return;
        char rn[8]; a_cpy(rn,av[0],sizeof rn);
        char*bang=rn; while(*bang&&*bang!='!')bang++; *bang=0;
        int n=reg(rn); if(n<0||n>7){err(c,"bad base reg");return;}
        int mask; if(!reglist(c,av[1],&mask))return;
        if(mask&~0xFF){err(c,"ldm/stm only r0-r7");return;}
        int ld=a_ieq(mnem,"LDMIA")||a_ieq(mnem,"LDM");
        emit16(c,(ld?0xC800:0xC000)|(n<<8)|(mask&0xFF));
        return;
    }

    /* --- MOV / MOVS --- */
    if(a_ieq(mnem,"MOV")||a_ieq(mnem,"MOVS")){
        if(!need(c,ac,2))return;
        int d=reg(av[0]);
        if(d<0){err(c,"bad reg");return;}
        int m=reg(av[1]);
        if(m>=0){                        /* MOV Rd, Rm */
            if(a_ieq(mnem,"MOVS")){       /* movs low regs = lsls #0 */
                if(d>7||m>7){err(c,"movs needs low regs");return;}
                emit16(c,0x0000|(m<<3)|d);
            } else emit16(c,0x4600|((d&8)<<4)|((m&15)<<3)|(d&7));  /* hi MOV: D bit at [7] */
            return;
        }
        /* MOV Rd, #imm8 */
        if(av[1][0]!='#'){err(c,"expected #imm or reg");return;}
        if(!imm(c,av[1]+1,&v))return;
        if(d>7){err(c,"mov imm needs low reg");return;}
        if(v<0||v>255){err(c,"imm out of range 0..255");return;}
        emit16(c,0x2000|(d<<8)|((int)v&0xFF));
        return;
    }
    if(a_ieq(mnem,"MVN")||a_ieq(mnem,"MVNS")){
        if(!need(c,ac,2))return; int d=reg(av[0]),m=reg(av[1]);
        if(d<0||m<0||d>7||m>7){err(c,"bad reg");return;}
        emit16(c,0x43C0|(m<<3)|d); return;
    }

    /* --- CMP --- */
    if(a_ieq(mnem,"CMP")){
        if(!need(c,ac,2))return; int n=reg(av[0]);
        if(n<0){err(c,"bad reg");return;}
        int m=reg(av[1]);
        if(m>=0){
            if(n<8&&m<8) emit16(c,0x4280|(m<<3)|n);       /* CMP lo,lo */
            else emit16(c,0x4500|((n&8)<<4)|((m&15)<<3)|(n&7)); /* CMP hi */
            return;
        }
        if(av[1][0]!='#'){err(c,"expected #imm or reg");return;}
        if(!imm(c,av[1]+1,&v))return;
        if(n>7){err(c,"cmp imm needs low reg");return;}
        if(v<0||v>255){err(c,"imm 0..255");return;}
        emit16(c,0x2800|(n<<8)|((int)v&0xFF)); return;
    }
    if(a_ieq(mnem,"CMN")){ if(!need(c,ac,2))return; int n=reg(av[0]),m=reg(av[1]);
        if(n<0||m<0||n>7||m>7){err(c,"bad reg");return;} emit16(c,0x42C0|(m<<3)|n); return; }
    if(a_ieq(mnem,"TST")){ if(!need(c,ac,2))return; int n=reg(av[0]),m=reg(av[1]);
        if(n<0||m<0||n>7||m>7){err(c,"bad reg");return;} emit16(c,0x4200|(m<<3)|n); return; }

    /* --- ADD / SUB (many forms) --- */
    if(a_ieq(mnem,"ADD")||a_ieq(mnem,"ADDS")||a_ieq(mnem,"SUB")||a_ieq(mnem,"SUBS")){
        int isadd=(mnem[0]=='A'||mnem[0]=='a');
        int d=reg(av[0]);
        if(d<0){err(c,"bad reg");return;}
        /* SP forms: ADD SP,SP,#imm7 ; ADD Rd,SP,#imm8 */
        if(d==13&&ac==3){
            int n=reg(av[1]);
            if(n==13&&av[2][0]=='#'){ if(!imm(c,av[2]+1,&v))return;
                if(v<0||v>508||(v&3)){err(c,"sp adj 0..508 /4");return;}
                emit16(c,(isadd?0xB000:0xB080)|(((int)v>>2)&0x7F)); return; }
        }
        if(ac==3){
            int n=reg(av[1]);
            int m=reg(av[2]);
            if(m>=0){                    /* ADD Rd,Rn,Rm */
                if(isadd){
                    if(d<8&&n<8&&m<8) emit16(c,0x1800|(m<<6)|(n<<3)|d);
                    else emit16(c,0x4400|((d&8)<<4)|((m&15)<<3)|(d&7)); /* hi add, Rd=Rn */
                } else {
                    if(d>7||n>7||m>7){err(c,"subs needs low regs");return;}
                    emit16(c,0x1A00|(m<<6)|(n<<3)|d);
                }
                return;
            }
            /* ADD Rd,Rn,#imm : imm3 or (Rd==Rn) imm8 */
            if(av[2][0]!='#'){err(c,"expected #imm/reg");return;}
            if(!imm(c,av[2]+1,&v))return;
            if(n==13){ /* ADD Rd, SP, #imm8*4 */
                if(!isadd){err(c,"no sub Rd,sp,#");return;}
                if(v<0||v>1020||(v&3)){err(c,"0..1020 /4");return;}
                emit16(c,0xA800|(d<<8)|(((int)v>>2)&0xFF)); return;
            }
            if(n<0){err(c,"bad reg");return;}
            if(d<8&&n<8&&v>=0&&v<=7)
                emit16(c,(isadd?0x1C00:0x1E00)|((int)v<<6)|(n<<3)|d);
            else if(d==n&&d<8&&v>=0&&v<=255)
                emit16(c,(isadd?0x3000:0x3800)|(d<<8)|((int)v&0xFF));
            else err(c,"add/sub imm out of range");
            return;
        }
        if(ac==2){
            /* ADD Rd,#imm8 ; ADD Rd,Rm */
            int m=reg(av[1]);
            if(m>=0){
                if(isadd) emit16(c,0x4400|((d&8)<<4)|((m&15)<<3)|(d&7)); /* ADD Rd,Rm (hi ok) */
                else { if(d>7||m>7){err(c,"sub needs low");return;} emit16(c,0x1A00|(m<<6)|(d<<3)|d); }
                return;
            }
            if(av[1][0]!='#'){err(c,"expected #imm/reg");return;}
            if(!imm(c,av[1]+1,&v))return;
            if(d>7||v<0||v>255){err(c,"imm 0..255 low reg");return;}
            emit16(c,(isadd?0x3000:0x3800)|(d<<8)|((int)v&0xFF));
            return;
        }
        err(c,"bad add/sub"); return;
    }

    /* --- shifts LSL/LSR/ASR (imm or reg), ROR (reg) --- */
    if(a_ieq(mnem,"LSL")||a_ieq(mnem,"LSLS")||a_ieq(mnem,"LSR")||a_ieq(mnem,"LSRS")||
       a_ieq(mnem,"ASR")||a_ieq(mnem,"ASRS")){
        int kind = (a_toupper((unsigned char)mnem[1])=='S')?0 : /* impossible */ 0;
        (void)kind;
        int base_imm = a_ieq(mnem,"LSL")||a_ieq(mnem,"LSLS") ? 0x0000 :
                       a_ieq(mnem,"LSR")||a_ieq(mnem,"LSRS") ? 0x0800 : 0x1000;
        int op_reg   = a_ieq(mnem,"LSL")||a_ieq(mnem,"LSLS") ? 2 :
                       a_ieq(mnem,"LSR")||a_ieq(mnem,"LSRS") ? 3 : 4;
        if(ac!=2&&ac!=3){err(c,"bad shift");return;}
        int d=reg(av[0]);
        int n = ac==3 ? reg(av[1]) : d;      /* 2-op form: Rd,Rd,op */
        const char*third = ac==3 ? av[2] : av[1];
        if(d<0||n<0||d>7){err(c,"bad reg");return;}
        int m=reg(third);
        if(m>=0){                        /* register shift: Rd==Rn */
            if(d!=n||m>7){err(c,"reg shift form Rd,Rd,Rs");return;}
            emit16(c,0x4000|(op_reg<<6)|(m<<3)|d); return;
        }
        if(third[0]!='#'){err(c,"expected #imm/reg");return;}
        if(!imm(c,third+1,&v))return;
        if(n>7||v<0||v>31){err(c,"shift 0..31");return;}
        emit16(c,base_imm|((int)v<<6)|(n<<3)|d); return;
    }
    if(a_ieq(mnem,"ROR")||a_ieq(mnem,"RORS")){
        if(ac!=2&&ac!=3){err(c,"bad ror");return;}
        int d=reg(av[0]); int n=ac==3?reg(av[1]):d; int m=reg(av[ac-1]);
        if(d<0||m<0||d!=n||d>7||m>7){err(c,"ror Rd,Rd,Rs");return;}
        emit16(c,0x41C0|(m<<3)|d); return;
    }

    /* --- data-processing register (AND EOR ADC SBC ORR BIC MUL) --- */
    {
        struct { const char*n; int op; } dp[]={
            {"AND",0},{"ANDS",0},{"EOR",1},{"EORS",1},{"ADC",5},{"ADCS",5},
            {"SBC",6},{"SBCS",6},{"ORR",12},{"ORRS",12},{"BIC",14},{"BICS",14},
            {"MUL",13},{"MULS",13},{0,0}};
        for(int i=0;dp[i].n;i++) if(a_ieq(mnem,dp[i].n)){
            if(!need(c,ac,ac>=3?3:2))return;
            int d=reg(av[0]),m;
            if(ac==3){ int n=reg(av[1]); m=reg(av[2]);
                if(dp[i].op==13){ /* MUL Rd,Rm,Rd (Rn is the multiplicand==Rd) */
                    if(d!=m&&d!=n){err(c,"mul Rd,Rn,Rd");return;} m=(d==m)?n:m;
                } else if(d!=n){ err(c,"dp form Rd,Rd,Rm");return; }
            } else m=reg(av[1]);
            if(d<0||m<0||d>7||m>7){err(c,"bad reg");return;}
            emit16(c,0x4000|(dp[i].op<<6)|(m<<3)|d); return;
        }
    }
    if(a_ieq(mnem,"NEG")||a_ieq(mnem,"NEGS")||a_ieq(mnem,"RSBS")){
        if(!need(c,ac,ac>=3?3:2))return; int d=reg(av[0]),m=reg(av[ac-1]);
        if(d<0||m<0||d>7||m>7){err(c,"bad reg");return;}
        emit16(c,0x4240|(m<<3)|d); return;
    }

    /* --- sign/zero extend, byte reverse --- */
    { struct{const char*n;int op;}ex[]={{"SXTH",0xB200},{"SXTB",0xB240},
        {"UXTH",0xB280},{"UXTB",0xB2C0},{"REV",0xBA00},{"REV16",0xBA40},
        {"REVSH",0xBAC0},{0,0}};
      for(int i=0;ex[i].n;i++) if(a_ieq(mnem,ex[i].n)){
        if(!need(c,ac,2))return; int d=reg(av[0]),m=reg(av[1]);
        if(d<0||m<0||d>7||m>7){err(c,"bad reg");return;}
        emit16(c,ex[i].op|(m<<3)|d); return; } }

    /* --- load/store --- */
    {
        struct{const char*n;int imm5,rege,sh,sp,pc;}ls[]={
          /* imm5-form opcode, reg-form opcode, imm shift, sp-form, pc-form */
          {"LDR", 0x6800,0x5800,2,0x9800,0x4800},
          {"STR", 0x6000,0x5000,2,0x9000,0},
          {"LDRB",0x7800,0x5C00,0,0,0},
          {"STRB",0x7000,0x5400,0,0,0},
          {"LDRH",0x8800,0x5A00,1,0,0},
          {"STRH",0x8000,0x5200,1,0,0},
          {"LDRSB",0,0x5600,0,0,0},
          {"LDRSH",0,0x5E00,0,0,0},
          {0,0,0,0,0,0}};
        for(int i=0;ls[i].n;i++) if(a_ieq(mnem,ls[i].n)){
            int t=reg(av[0]);
            if(t<0){err(c,"bad reg");return;}
            /* LDR Rt, =expr  (literal pool) */
            if(ac==2&&av[1][0]=='='){
                if(ls[i].pc==0){err(c,"only LDR supports =literal");return;}
                if(t>7){err(c,"ldr literal low reg");return;}
                /* record literal; resolve offset in pass 2 */
                long lv=0; int is_sym=0; char sname[ASM_MAX_LABELLEN+1]={0};
                const char*e=skipws(av[1]+1);
                if(a_isid0((unsigned char)*e)&&!a_isdigit((unsigned char)*e)){
                    int k=0; while(a_isid((unsigned char)*e)&&k<ASM_MAX_LABELLEN)sname[k++]=*e++; sname[k]=0;
                    is_sym=1;
                } else if(!eval(c,av[1]+1,&lv)) return;
                if(c->nlit<ASM_MAX_LITERALS){
                    c->lit[c->nlit].value=(uint32_t)lv;
                    c->lit[c->nlit].is_sym=is_sym;
                    a_cpy(c->lit[c->nlit].sym,sname,sizeof c->lit[0].sym);
                    c->lit[c->nlit].patch_at=c->code_len;
                    c->nlit++;
                }
                emit16(c,ls[i].pc|(t<<8));   /* offset patched in pass 2 */
                return;
            }
            if(ac!=2){err(c,"ldr/str Rt,[...]");return;}
            /* parse [Rn], [Rn,#imm], [Rn,Rm] */
            char inb[64]; a_cpy(inb,av[1],sizeof inb);
            char*p=inb; while(*p&&*p!='[')p++;
            if(*p!='['){err(c,"expected [");return;}
            p++;
            char*rb=p; while(*p&&*p!=','&&*p!=']')p++;
            char save=*p; *p=0;
            int n=reg(rb);
            /* PC/SP-relative */
            char*rest = (save==',')?p+1:p;
            if(save==','){
                char*q=rest; while(*q&&*q!=']')q++; *q=0;
                q=rest; q=(char*)skipws(q);
                if(q[0]=='#'){
                    if(!imm(c,q+1,&v))return;
                    if(n==13&&ls[i].sp){ if(v<0||v>1020||(v&3)){err(c,"sp off 0..1020");return;}
                        emit16(c,ls[i].sp|(t<<8)|(((int)v>>2)&0xFF)); return; }
                    int sh=ls[i].sh; int maxoff=31<<sh;
                    if(t>7||n<0||n>7){err(c,"low regs only");return;}
                    if(v<0||v>maxoff||((int)v&((1<<sh)-1))){err(c,"offset range/align");return;}
                    emit16(c,ls[i].imm5|(((int)v>>sh)<<6)|(n<<3)|t); return;
                } else {
                    int m=reg(q);
                    if(m<0||t>7||n<0||n>7||m>7){err(c,"bad reg offset");return;}
                    if(!ls[i].rege){err(c,"no reg-offset form");return;}
                    emit16(c,ls[i].rege|(m<<6)|(n<<3)|t); return;
                }
            } else {
                /* [Rn] alone → offset 0 */
                if(n==13&&ls[i].sp){ emit16(c,ls[i].sp|(t<<8)); return; }
                if(t>7||n<0||n>7){err(c,"low regs only");return;}
                emit16(c,ls[i].imm5|(n<<3)|t); return;
            }
        }
    }

    /* --- ADR Rd, label  → ADD Rd, PC, #off --- */
    if(a_ieq(mnem,"ADR")){
        if(!need(c,ac,2))return; int d=reg(av[0]);
        if(d<0||d>7){err(c,"adr low reg");return;}
        if(!eval(c,av[1],&v))return;
        uint32_t base=((c->pc+4)&~3u);
        long off=v-(long)base;
        if(off<0||off>1020||(off&3)){ if(c->pass==2)err(c,"adr range/align"); off=off<0?0:off; }
        emit16(c,0xA000|(d<<8)|(((int)off>>2)&0xFF)); return;
    }

    /* --- branches --- */
    if(a_ieq(mnem,"B")){
        if(!need(c,ac,1))return; if(!eval(c,av[0],&v))return;
        long off=v-((long)c->pc+4);
        if(off&1){err(c,"odd branch");return;}
        long h=off>>1;
        if(h< -1024||h>1023){ if(c->pass==2)err(c,"branch out of range"); }
        emit16(c,0xE000|((int)h&0x7FF)); return;
    }
    if((mnem[0]=='B'||mnem[0]=='b')&&mnem[1]){
        int cc=cond_code(mnem+1);
        if(cc>=0){
            if(!need(c,ac,1))return; if(!eval(c,av[0],&v))return;
            long off=v-((long)c->pc+4);
            if(off&1){err(c,"odd branch");return;}
            long h=off>>1;
            if(cc==14){ if(h<-1024||h>1023){if(c->pass==2)err(c,"b range");} emit16(c,0xE000|((int)h&0x7FF)); return; }
            if(h< -128||h>127){ if(c->pass==2)err(c,"bcc out of range"); }
            emit16(c,0xD000|(cc<<8)|((int)h&0xFF)); return;
        }
    }
    if(a_ieq(mnem,"BL")){
        if(!need(c,ac,1))return; if(!eval(c,av[0],&v))return;
        long off=v-((long)c->pc+4);
        if(off&1){err(c,"odd bl");return;}
        int S=(off>>24)&1, I1=(off>>23)&1, I2=(off>>22)&1;
        int imm10=(off>>12)&0x3FF, imm11=(off>>1)&0x7FF;
        int J1=(~(I1^S))&1, J2=(~(I2^S))&1;
        emit16(c,0xF000|(S<<10)|imm10);
        emit16(c,0xD000|(J1<<13)|(J2<<11)|imm11);
        return;
    }

    err(c,"unknown mnemonic");
}

/*==========================================================================
 * Directives
 *=========================================================================*/
static void align_to(asm_ctx*c,int a){
    if(a<=1) return;
    while(c->pc%(unsigned)a){ emit8(c,0); }
}

/* returns 1 if `mnem` was a directive */
static int directive(asm_ctx*c,const char*mnem,char*ops){
    char*av[16]; char obuf[160]; long v;
    if(mnem[0]!='.') return 0;
    a_cpy(obuf,ops,sizeof obuf);
    int ac=split_ops(obuf,av,16);

    if(a_ieq(mnem,".org")){ if(ac>=1&&eval(c,av[0],&v)){ c->org=(uint32_t)v; c->pc=(uint32_t)v; } return 1; }
    if(a_ieq(mnem,".align")){ int a=4; if(ac>=1&&eval(c,av[0],&v)) a=1<<(int)v; align_to(c,a); return 1; }
    if(a_ieq(mnem,".balign")){ int a=4; if(ac>=1&&eval(c,av[0],&v)) a=(int)v; align_to(c,a); return 1; }
    if(a_ieq(mnem,".word")||a_ieq(mnem,".int")||a_ieq(mnem,".long")){
        for(int i=0;i<ac;i++){ v=0; eval(c,av[i],&v);
            if(c->pass==2&&c->expr_used_sym&&c->nreloc<ASM_MAX_RELOCS)
                c->reloc[c->nreloc++]=c->code_len;   /* R_ARM_ABS32 here */
            emit8(c,v&0xFF); emit8(c,(v>>8)&0xFF); emit8(c,(v>>16)&0xFF); emit8(c,(v>>24)&0xFF);} return 1; }
    if(a_ieq(mnem,".hword")||a_ieq(mnem,".short")||a_ieq(mnem,".half")){
        for(int i=0;i<ac;i++){ v=0; eval(c,av[i],&v); emit8(c,v&0xFF); emit8(c,(v>>8)&0xFF);} return 1; }
    if(a_ieq(mnem,".byte")){ for(int i=0;i<ac;i++){ v=0; eval(c,av[i],&v); emit8(c,v&0xFF);} return 1; }
    if(a_ieq(mnem,".space")||a_ieq(mnem,".skip")){ v=0; if(ac>=1)eval(c,av[0],&v); long fill=0; if(ac>=2)eval(c,av[1],&fill); for(long i=0;i<v;i++)emit8(c,fill&0xFF); return 1; }
    if(a_ieq(mnem,".ascii")||a_ieq(mnem,".asciz")||a_ieq(mnem,".string")){
        /* single quoted-string operand */
        char*p=ops; while(*p&&*p!='"')p++;
        if(*p=='"'){ p++; while(*p&&*p!='"'){ int ch=*p++;
            if(ch=='\\'&&*p){ char e=*p++; ch = e=='n'?'\n':e=='t'?'\t':e=='r'?'\r':e=='0'?0:e; }
            emit8(c,ch); } }
        if(!a_ieq(mnem,".ascii")) emit8(c,0);
        return 1;
    }
    if(a_ieq(mnem,".equ")||a_ieq(mnem,".set")){
        if(ac>=2){ asm_symbol*s=sym_add(c,av[0]); if(s&&eval(c,av[1],&v)){ s->value=(uint32_t)v; s->defined=1; s->is_equ=1; } }
        return 1;
    }
    if(a_ieq(mnem,".global")||a_ieq(mnem,".globl")||a_ieq(mnem,".thumb")||
       a_ieq(mnem,".syntax")||a_ieq(mnem,".text")||a_ieq(mnem,".type")||
       a_ieq(mnem,".cpu")||a_ieq(mnem,".arch")||a_ieq(mnem,".thumb_func")||
       a_ieq(mnem,".code")||a_ieq(mnem,".section")||a_ieq(mnem,".size")||
       a_ieq(mnem,".p2align")||a_ieq(mnem,".ltorg")||a_ieq(mnem,".pool")||
       a_ieq(mnem,".fpu")||a_ieq(mnem,".eabi_attribute")){
        return 1;                       /* accepted & ignored (pool is
                                           auto-emitted at end of image) */
    }
    err(c,"unknown directive");
    return 1;
}

/*==========================================================================
 * Line processing
 *=========================================================================*/
/* Process one logical line (already NUL-terminated, no newline). */
static void do_line(asm_ctx*c,char*line){
    /* strip comments: ';', '@', and '//' (but not inside a string) */
    int instr=0;
    for(char*p=line;*p;p++){
        if(*p=='"') instr^=1;
        else if(!instr&&(*p==';'||*p=='@')){ *p=0; break; }
        else if(!instr&&p[0]=='/'&&p[1]=='/'){ *p=0; break; }
    }
    char*p=line; p=(char*)skipws(p);
    if(!*p) return;

    /* leading label(s): "name:" possibly repeated */
    for(;;){
        /* find a label = identifier followed by ':' */
        char*q=p; if(!a_isid0((unsigned char)*q)) break;
        char*e=q; while(a_isid((unsigned char)*e)) e++;
        char*e2=(char*)skipws(e);
        if(*e2!=':') break;
        char nm[ASM_MAX_LABELLEN+1]; int n=0;
        for(char*s=q;s<e&&n<ASM_MAX_LABELLEN;s++) nm[n++]=*s; nm[n]=0;
        if(c->pass==1){
            asm_symbol*s=sym_add(c,nm);
            if(s){ if(s->defined&&!s->is_equ) err(c,"duplicate label");
                   s->value=c->pc; s->defined=1; }
            if(a_streq(nm,"main")||a_streq(nm,"_start")) c->entry=c->pc-c->org;
        }
        p=(char*)skipws(e2+1);
        if(!*p) return;                  /* label-only line */
    }

    /* mnemonic */
    char mnem[24]; int n=0;
    while((a_isid((unsigned char)*p))&&n<23) mnem[n++]=*p++;
    mnem[n]=0;
    if(!n){ err(c,"expected instruction"); return; }
    p=(char*)skipws(p);

    if(mnem[0]=='.'){ directive(c,mnem,p); return; }
    encode(c,mnem,p);
}

/*==========================================================================
 * Literal pool — appended after the last instruction, word aligned.
 * Deduplicated deterministically so pass 1 and pass 2 agree on size.
 *=========================================================================*/
static void emit_literals(asm_ctx*c){
    if(c->nlit==0) return;
    align_to(c,4);
    /* assign each unique literal an address; dedup by (is_sym,value/sym) */
    for(int i=0;i<c->nlit;i++){
        /* find first identical earlier literal */
        int first=i;
        for(int j=0;j<i;j++){
            if(c->lit[j].is_sym==c->lit[i].is_sym &&
               ((c->lit[i].is_sym && a_streq(c->lit[j].sym,c->lit[i].sym)) ||
                (!c->lit[i].is_sym && c->lit[j].value==c->lit[i].value))){ first=j; break; }
        }
        if(first!=i) continue;           /* will reuse first's slot */
        /* this literal's address is current pc */
        uint32_t addr=c->pc;
        long val=c->lit[i].value;
        if(c->lit[i].is_sym){ asm_symbol*s=sym_find(c,c->lit[i].sym);
            if(s&&s->defined) val=(long)s->value; else if(c->pass==2) err(c,"undef literal sym"); }
        /* patch every ldr that referenced this literal */
        if(c->pass==2){
            for(int k=0;k<c->nlit;k++){
                int fk=k;
                for(int j=0;j<k;j++) if(c->lit[j].is_sym==c->lit[k].is_sym &&
                    ((c->lit[k].is_sym&&a_streq(c->lit[j].sym,c->lit[k].sym))||
                     (!c->lit[k].is_sym&&c->lit[j].value==c->lit[k].value))){ fk=j; break; }
                if(fk!=i) continue;
                uint32_t at=c->lit[k].patch_at;
                uint32_t ldrpc=((c->org+at)+4)&~3u;
                long off=(long)addr-(long)ldrpc;   /* addr is org+pc already? see below */
                /* addr computed from c->pc which is org-relative base; make absolute */
                off=(long)(c->org+ (addr-c->org)) - (long)ldrpc;
                if(off<0||off>1020||(off&3)) err(c,"literal out of range");
                int hw=c->code[at]|(c->code[at+1]<<8);
                hw=(hw&0xFF00)|(((int)off>>2)&0xFF);
                c->code[at]=hw&0xFF; c->code[at+1]=(hw>>8)&0xFF;
            }
        }
        if(c->pass==2&&c->lit[i].is_sym&&c->nreloc<ASM_MAX_RELOCS)
            c->reloc[c->nreloc++]=c->code_len;   /* R_ARM_ABS32 on this word */
        emit8(c,val&0xFF); emit8(c,(val>>8)&0xFF); emit8(c,(val>>16)&0xFF); emit8(c,(val>>24)&0xFF);
    }
}

/*==========================================================================
 * Two-pass driver
 *=========================================================================*/
static void run_pass(asm_ctx*c,const char*src,int pass){
    c->pass=pass; c->pc=c->org; c->code_len=0; c->cur_line=0; c->nreloc=0;
    /* keep nlit across passes? reset patch tracking but keep entries from
     * pass 1 count; simplest: rebuild each pass */
    c->nlit=0;
    char line[256]; int li=0; c->cur_line=1;
    for(const char*s=src;;s++){
        char ch=*s;
        if(ch=='\n'||ch==0){
            line[li]=0;
            do_line(c,line);
            li=0; c->cur_line++;
            if(ch==0) break;
        } else if(li<(int)sizeof(line)-1){
            line[li++]=ch;
        }
    }
    emit_literals(c);
}

int asm_assemble(asm_ctx*c,const char*src){
    /* zero everything the caller may not have */
    c->code_len=0; c->org=0; c->entry=0; c->nsym=0; c->nerr=0; c->nlit=0;
    c->nreloc=0; c->expr_used_sym=0;
    run_pass(c,src,1);
    /* pass 2 with symbols resolved */
    int saved_org=c->org;
    (void)saved_org;
    run_pass(c,src,2);
    return c->nerr? -1 : 0;
}

const asm_symbol* asm_find_symbol(const asm_ctx*c,const char*name){
    for(int i=0;i<c->nsym;i++) if(a_streq(c->sym[i].name,name)) return &c->sym[i];
    return 0;
}

/*==========================================================================
 * CSUB text emitter (format per tools/csub/mkcsub.sh)
 *   CSUB <name> <types>
 *     00000000                       <- entry word offset (see note)
 *     <hex words, 4 per line, big-endian-printed little-endian words>
 *   END CSUB
 * Note: MMBasic's CSUB layout is [size][entry-word-offset][thumb...].
 * mkcsub emits the entry offset line then the code words.  Here entry
 * is ctx->entry (bytes) >> 2.
 *=========================================================================*/
static char* put(char*o,char*end,const char*s){ while(*s&&o<end) *o++=*s++; return o; }
static char* puthex8(char*o,char*end,uint32_t w){
    const char*h="0123456789ABCDEF";
    for(int i=28;i>=0;i-=4) if(o<end) *o++=h[(w>>i)&0xF];
    return o;
}

int asm_emit_csub(const asm_ctx*c,const char*name,const char*types,char*out,size_t cap){
    char*o=out,*end=out+cap;
    o=put(o,end,"CSUB "); o=put(o,end,name);
    if(types&&types[0]){ o=put(o,end," "); o=put(o,end,types); }
    o=put(o,end,"\n");
    /* entry word offset line */
    o=put(o,end,"  "); o=puthex8(o,end,c->entry>>2); o=put(o,end,"\n");
    /* code words, little-endian, 4 per line */
    uint32_t n=(c->code_len+3)&~3u;
    for(uint32_t i=0;i<n;i+=4){
        if((i&15)==0) o=put(o,end,"  ");
        uint32_t w = (uint32_t)c->code[i] |
            ((i+1<c->code_len?(uint32_t)c->code[i+1]:0)<<8) |
            ((i+2<c->code_len?(uint32_t)c->code[i+2]:0)<<16) |
            ((i+3<c->code_len?(uint32_t)c->code[i+3]:0)<<24);
        o=puthex8(o,end,w);
        if((i&15)==12 || i+4>=n) o=put(o,end,"\n"); else o=put(o,end," ");
    }
    o=put(o,end,"END CSUB\n");
    if(o>=end) return -1;
    *o=0;
    return (int)(o-out);
}

/*==========================================================================
 * ELF32 relocatable object emitter (ET_REL, EM_ARM) for the FRANK OS
 * loader.  Sections: NULL, .text, .rel.text, .symtab, .strtab, .shstrtab.
 * Symbols: null, .text section symbol (local), <entry> as GLOBAL FUNC
 * (thumb: st_value bit0 set).  All relocations are R_ARM_ABS32 against
 * the .text section symbol, with the in-place word holding the addend
 * (target offset), so the loader adds only the .text load base.
 *=========================================================================*/
static void le16(uint8_t*p,unsigned v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void le32(uint8_t*p,uint32_t v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

int asm_emit_elf(const asm_ctx*c,const char*entry_name,uint8_t*out,size_t cap){
    if(!entry_name) entry_name="main";
    int enl=a_strlen(entry_name);

    /* --- string tables --- */
    /* .strtab: "\0<entry>\0" */
    uint8_t strtab[ASM_MAX_LABELLEN+4]; int strtab_len=0;
    strtab[strtab_len++]=0;
    int entry_stroff=strtab_len;
    for(int i=0;i<enl;i++) strtab[strtab_len++]=(uint8_t)entry_name[i];
    strtab[strtab_len++]=0;

    /* .shstrtab */
    static const char shs[]="\0.text\0.rel.text\0.symtab\0.strtab\0.shstrtab";
    int shs_len=(int)sizeof(shs);          /* includes trailing NUL */
    const int NM_TEXT=1, NM_REL=7, NM_SYMTAB=17, NM_STRTAB=25, NM_SHSTR=33;

    /* --- symbol table (3 entries * 16 bytes) --- */
    const int NSYM=3;
    uint8_t symtab[3*16]; for(int i=0;i<3*16;i++) symtab[i]=0;
    /* [1] .text section symbol: st_info=STT_SECTION(3), shndx=1 */
    symtab[16+12]=3;                       /* st_info at offset 12 */
    le16(&symtab[16+14],1);                /* st_shndx = .text (1) */
    /* [2] entry: global func, value=entry|1 (thumb), shndx=1 */
    le32(&symtab[32+0],(uint32_t)entry_stroff);        /* st_name */
    le32(&symtab[32+4],(c->entry|1u));                 /* st_value (thumb) */
    le32(&symtab[32+8],0);                             /* st_size */
    symtab[32+12]=0x12;                                /* GLOBAL|FUNC */
    le16(&symtab[32+14],1);                            /* st_shndx=.text */
    int symtab_len=NSYM*16;

    /* --- relocations (nreloc * 8) : R_ARM_ABS32 vs sym #1 (.text) --- */
    int rel_len=c->nreloc*8;

    /* --- file layout --- */
    uint32_t off=52;                       /* after ehdr */
    uint32_t text_off=off;                  off+=c->code_len; off=(off+3)&~3u;
    uint32_t rel_off =off;                  off+=(uint32_t)rel_len; off=(off+3)&~3u;
    uint32_t sym_off =off;                  off+=(uint32_t)symtab_len; off=(off+3)&~3u;
    uint32_t str_off =off;                  off+=(uint32_t)strtab_len;
    uint32_t shs_off =off;                  off+=(uint32_t)shs_len; off=(off+3)&~3u;
    uint32_t sht_off =off;                  off+=6*40;

    if(off>cap) return -1;
    for(uint32_t i=0;i<off;i++) out[i]=0;

    /* --- ELF header --- */
    out[0]=0x7F; out[1]='E'; out[2]='L'; out[3]='F';
    out[4]=1;  /* 32-bit */ out[5]=1; /* LE */ out[6]=1; /* version */
    le16(&out[16],1);          /* e_type = ET_REL */
    le16(&out[18],40);         /* e_machine = EM_ARM */
    le32(&out[20],1);          /* e_version */
    le32(&out[24],0);          /* e_entry */
    le32(&out[28],0);          /* e_phoff */
    le32(&out[32],sht_off);    /* e_shoff */
    le32(&out[36],0x05000000); /* e_flags = EABI v5 */
    le16(&out[40],52);         /* e_ehsize */
    le16(&out[42],0);          /* e_phentsize */
    le16(&out[44],0);          /* e_phnum */
    le16(&out[46],40);         /* e_shentsize */
    le16(&out[48],6);          /* e_shnum */
    le16(&out[50],5);          /* e_shstrndx */

    /* --- section contents --- */
    for(uint32_t i=0;i<c->code_len;i++) out[text_off+i]=c->code[i];
    for(int i=0;i<c->nreloc;i++){
        le32(&out[rel_off+i*8+0],c->reloc[i]);        /* r_offset */
        le32(&out[rel_off+i*8+4],(1u<<8)|2u);         /* sym=1, type=R_ARM_ABS32 */
    }
    for(int i=0;i<symtab_len;i++) out[sym_off+i]=symtab[i];
    for(int i=0;i<strtab_len;i++) out[str_off+i]=strtab[i];
    for(int i=0;i<shs_len;i++)    out[shs_off+i]=(uint8_t)shs[i];

    /* --- section header table (6 * 40 bytes) --- */
    uint8_t*sh=&out[sht_off];
    /* [0] NULL — all zero */
    /* [1] .text */
    le32(&sh[40+0],NM_TEXT); le32(&sh[40+4],1 /*PROGBITS*/);
    le32(&sh[40+8],2|4 /*ALLOC|EXECINSTR*/); le32(&sh[40+16],text_off);
    le32(&sh[40+20],c->code_len); le32(&sh[40+32],4 /*align*/);
    /* [2] .rel.text */
    le32(&sh[80+0],NM_REL); le32(&sh[80+4],9 /*SHT_REL*/);
    le32(&sh[80+16],rel_off); le32(&sh[80+20],(uint32_t)rel_len);
    le32(&sh[80+24],3 /*sh_link=.symtab idx*/); le32(&sh[80+28],1 /*sh_info=.text idx*/);
    le32(&sh[80+32],4); le32(&sh[80+36],8 /*entsize*/);
    /* [3] .symtab */
    le32(&sh[120+0],NM_SYMTAB); le32(&sh[120+4],2 /*SYMTAB*/);
    le32(&sh[120+16],sym_off); le32(&sh[120+20],(uint32_t)symtab_len);
    le32(&sh[120+24],4 /*sh_link=.strtab idx*/); le32(&sh[120+28],2 /*first global*/);
    le32(&sh[120+32],4); le32(&sh[120+36],16 /*entsize*/);
    /* [4] .strtab */
    le32(&sh[160+0],NM_STRTAB); le32(&sh[160+4],3 /*STRTAB*/);
    le32(&sh[160+16],str_off); le32(&sh[160+20],(uint32_t)strtab_len);
    le32(&sh[160+32],1);
    /* [5] .shstrtab */
    le32(&sh[200+0],NM_SHSTR); le32(&sh[200+4],3 /*STRTAB*/);
    le32(&sh[200+16],shs_off); le32(&sh[200+20],(uint32_t)shs_len);
    le32(&sh[200+32],1);

    return (int)off;
}
