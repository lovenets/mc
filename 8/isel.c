#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "parse.h"
#include "gen.h"
#include "asm.h"

/* instruction selection state */
typedef struct Isel Isel;
struct Isel {
    Insn **il;
    size_t ni;
    Htab *locs; /* Node => int stkoff */
};

/* string tables */
char *regnames[] = {
#define Reg(r, name, mode) name,
#include "regs.def"
#undef Reg
};

Mode regmodes[] = {
#define Reg(r, name, mode) mode,
#include "regs.def"
#undef Reg
};

char *insnfmts[] = {
#define Insn(val, fmt, attr) fmt,
#include "insns.def"
#undef Insn
};

/* used to decide which operator is appropriate
 * for implementing various conditional operators */
struct {
    AsmOp test; 
    AsmOp jmp;
    AsmOp getflag;
} reloptab[Numops] = {
    [Oeq] = {Itest, Ijnz, Isetnz},
    [One] = {Itest, Ijz, Isetz},
    [Ogt] = {Icmp, Ijgt, Isetgt},
    [Oge] = {Icmp, Ijge, Isetge},
    [Olt] = {Icmp, Ijlt, Isetlt},
    [Ole] = {Icmp, Ijle, Isetle}
};


/* forward decls */
Loc selexpr(Isel *s, Node *n);


Loc *loclbl(Loc *l, char *lbl)
{
    l->type = Loclbl;
    l->mode = ModeL;
    l->lbl = strdup(lbl);
    return l;
}

Loc *locreg(Loc *l, Reg r)
{
    l->type = Locreg;
    l->mode = regmodes[r];
    l->reg = r;
    return l;
}

Loc *locmem(Loc *l, long disp, Reg base, Reg idx, Mode mode)
{
    l->type = Locmem;
    l->mode = mode;
    l->mem.constdisp = disp;
    l->mem.base = base;
    l->mem.idx = idx;
    return l;
}

Loc *locmeml(Loc *l, char *disp, Reg base, Reg idx, Mode mode)
{
    l->type = Locmem;
    l->mode = mode;
    l->mem.lbldisp = strdup(disp);
    l->mem.base = base;
    l->mem.idx = idx;
    return l;
}

Loc *loclit(Loc *l, long val)
{
    l->type = Loclit;
    l->mode = ModeL; /* FIXME: what do we do for mode? */
    l->lit = val;
    return l;
}

Loc loc(Isel *s, Node *n)
{
    Loc l;
    Node *v;
    switch (exprop(n)) {
        case Ovar:
            locmem(&l, 0, Reax, Rnone, ModeL);
            break;
        case Olit:
            v = n->expr.args[0];
            switch (v->lit.littype) {
                case Lchr:      loclit(&l, v->lit.chrval); break;
                case Lbool:     loclit(&l, v->lit.boolval); break;
                case Lint:      loclit(&l, v->lit.intval); break;
                default:
                    die("Literal type %s should be blob", litstr(v->lit.littype));
            }
            break;
        default:
            die("Node %s not leaf in loc()", opstr(exprop(n)));
            break;
    }
    return l;
}

Mode mode(Node *n)
{
    return ModeL;
}

Loc coreg(Loc r, Mode m)
{
    Loc l;

    Reg crtab[][4] = {
        [Ral] = {Ral, Rax, Reax},
        [Rcl] = {Rcl, Rcx, Recx},
        [Rdl] = {Rdl, Rdx, Redx},
        [Rbl] = {Rbl, Rbx, Rebx},

        [Rax] = {Ral, Rax, Reax},
        [Rcx] = {Rcl, Rcx, Recx},
        [Rdx] = {Rdl, Rdx, Redx},
        [Rbx] = {Rbl, Rbx, Rebx},
        [Rsi] = {0, Rsi, Resi},
        [Rdi] = {0, Rdi, Redi},

        [Reax] = {Ral, Rax, Reax},
        [Recx] = {Rcl, Rcx, Recx},
        [Redx] = {Rdl, Rdx, Redx},
        [Rebx] = {Rbl, Rbx, Rebx},
        [Resi] = {0, Rsi, Resi},
        [Redi] = {0, Rdi, Redi},
    };
    if (r.type != Locreg)
        die("Non-reg passed to coreg()");
    locreg(&l, crtab[r.reg][m]);
    return l;
}

Loc getreg(Isel *s, Mode m)
{
    Loc l;
    locreg(&l, Reax);
    return l;
}

Insn *mkinsnv(AsmOp op, va_list ap)
{
    Loc *l;
    Insn *i;
    int n;

    n = 0;
    i = malloc(sizeof(Insn));
    i->op = op;
    while ((l = va_arg(ap, Loc*)) != NULL)
        i->args[n++] = *l;
    i->narg = n;
    return i;
}

Insn *mkinsn(AsmOp op, ...)
{
    va_list ap;
    Insn *i;

    va_start(ap, op);
    i = mkinsnv(op, ap);
    va_end(ap);
    return i;
}

void g(Isel *s, AsmOp op, ...)
{
    va_list ap;
    Insn *i;

    va_start(ap, op);
    i = mkinsnv(op, ap);
    va_end(ap);
    lappend(&s->il, &s->ni, i);
}

void mov(Isel *s, Loc *a, Loc *b)
{
    if (a->type == Locreg && b->type == Locreg)
        if (a->reg == b->reg)
            return;
    g(s, Imov, a, b, NULL);
}


/* If we're testing equality, etc, it's a bit silly
 * to generate the test, store it to a bite, expand it
 * to the right width, and then test it again. Try to optimize
 * for these common cases.
 *
 * if we're doing the optimization to avoid
 * multiple tests, we want to eval the children
 * of the first arg, instead of the first arg
 * directly */
void selcjmp(Isel *s, Node *n, Node **args)
{
    Loc a, b;
    Loc l1, l2;
    AsmOp cond, jmp;

    cond = reloptab[exprop(n)].test;
    jmp = reloptab[exprop(n)].jmp;
    /* if we have a cond, we're knocking off the redundant test,
     * and want to eval the children */
    if (cond) {
        a = selexpr(s, args[0]->expr.args[0]);
        b = selexpr(s, args[0]->expr.args[1]);
    } else {
        cond = Itest;
        jmp = Ijnz;
        a = selexpr(s, args[0]); /* cond */
        b = a;
    }

    /* the jump targets will always be evaluated the same way */
    l1 = selexpr(s, args[1]); /* if true */
    l2 = selexpr(s, args[2]); /* if false */

    g(s, cond, &a, &b, NULL);
    g(s, jmp, &l1, NULL);
    g(s, Ijmp, &l2, NULL);
}

Loc selexpr(Isel *s, Node *n)
{
    Loc a, b, c, r;
    Node **args;

    args = n->expr.args;
    r = (Loc){Locnone, };
    switch (exprop(n)) {
        case Oadd:
            a = selexpr(s, args[0]);
            b = selexpr(s, args[1]);
            g(s, Iadd, &b, &a, NULL);
            r = b;
            break;

        case Osub:
            a = selexpr(s, args[0]);
            b = selexpr(s, args[1]);
            g(s, Iadd, &b, &a, NULL);
            r = b;
            break;

        case Omul: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Odiv: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Omod: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Oneg: die("Unimplemented op %s", opstr(exprop(n))); break;

        case Obor: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Oband: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Obxor: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Obsl: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Obsr: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Obnot: die("Unimplemented op %s", opstr(exprop(n))); break;

        case Oaddr: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Oderef: die("Unimplemented op %s", opstr(exprop(n))); break;

        case Oeq: case One: case Ogt: case Oge: case Olt: case Ole:
            a = selexpr(s, args[0]);
            b = selexpr(s, args[1]);
            c = getreg(s, ModeB);
            r = coreg(c, mode(n));
            g(s, reloptab[exprop(n)].test, &a, &b, NULL);
            g(s, reloptab[exprop(n)].getflag, &c, NULL);
            g(s, Imovz, &c, &r);
            return r;

        case Oasn:
            a = selexpr(s, args[0]);
            b = selexpr(s, args[1]);
            mov(s, &b, &a);
            r = b;
            break;

        case Ocall: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Ocast: die("Unimplemented op %s", opstr(exprop(n))); break;
        case Ojmp:
            g(s, Ijmp, loclbl(&a, args[0]->lbl.name), NULL);
            break;
        case Ocjmp:
            selcjmp(s, n, args);
            break;

        case Olit: /* fall through */
        case Ovar:
            b = loc(s, n);
            a = getreg(s, mode(args[0]));
            mov(s, &b, &a);
            r = b;
            break;
        case Olbl:
            loclbl(&r, args[0]->lbl.name);
            break;

        /* These operators should never show up in the reduced trees,
         * since they should have been replaced with more primitive
         * expressions by now */
        case Obad: case Oret: case Opreinc: case Opostinc: case Opredec:
        case Opostdec: case Olor: case Oland: case Olnot: case Oaddeq:
        case Osubeq: case Omuleq: case Odiveq: case Omodeq: case Oboreq:
        case Obandeq: case Obxoreq: case Obsleq: case Obsreq: case Omemb:
        case Oslice: case Oidx: case Osize: case Numops:
            die("Should not see %s in isel", opstr(exprop(n)));
            break;
    }
    return r;
}

void locprint(FILE *fd, Loc *l)
{

    switch (l->type) {
        case Loclbl:
            fprintf(fd, "%s", l->lbl);
            break;
        case Locreg:
            fprintf(fd, "%s", regnames[l->reg]);
            break;
        case Locmem:
        case Locmeml:
            if (l->type == Locmem) {
                if (l->mem.constdisp)
                    fprintf(fd, "%ld", l->mem.constdisp);
            } else {
                if (l->mem.lbldisp)
                    fprintf(fd, "%s", l->mem.lbldisp);
            }
            if (l->mem.base)
                fprintf(fd, "(%s", regnames[l->mem.base]);
            if (l->mem.idx)
                fprintf(fd, ",%s", regnames[l->mem.idx]);
            if (l->mem.base)
                fprintf(fd, ")");
            break;
        case Loclit:
            fprintf(fd, "$%ld", l->lit);
            break;
        case Locnone:
            die("Bad location in locprint()");
            break;
    }
}

void modeprint(FILE *fd, Loc *l)
{
    char mode[] = {'b', 's', 'l', 'q'};
    fputc(mode[l->mode], fd);
}

void iprintf(FILE *fd, Insn *insn)
{
    char *p;
    int i;
    int modeidx;
    
    p = insnfmts[insn->op];
    i = 0;
    for (; *p; p++) {
        if (*p !=  '%') {
            fputc(*p, fd);
            continue;
        }

        /* %-formating */
        p++;
        switch (*p) {
            case '\0':
                goto done;
            case 'r':
            case 'm':
            case 'l':
            case 'x':
            case 'v':
                locprint(fd, &insn->args[i]);
                i++;
                break;
            case 't':
                modeidx = 0;
            default:
                if (isdigit(*p))
                    modeidx = strtol(p, &p, 10);

                if (*p == 't')
                    modeprint(fd, &insn->args[modeidx]);
                else
                    die("Invalid %%-specifier '%c'", *p);
                break;
        }
    }
done:
    return;
}

void isel(Isel *s, Node *n)
{
    Loc lbl;

    switch (n->type) {
        case Nlbl:
            g(s, Ilbl, loclbl(&lbl, n->lbl.name), NULL);
            break;
        case Nexpr:
            selexpr(s, n);
            break;
        case Ndecl:
            break;
        default:
            die("Bad node type in isel()");
            break;
    }
}

void prologue(Isel *s, size_t sz)
{
    Loc esp;
    Loc ebp;
    Loc stksz;

    locreg(&esp, Resp);
    locreg(&ebp, Rebp);
    loclit(&stksz, sz);
    g(s, Ipush, &ebp, NULL);
    g(s, Imov, &esp, &ebp, NULL);
    g(s, Isub, &stksz, &esp, NULL);
}

void epilogue(Isel *s)
{
    Loc esp;
    Loc ebp;
    Loc stksz;

    locreg(&esp, Resp);
    locreg(&ebp, Rebp);
    loclit(&stksz, 16);
    g(s, Imov, &ebp, &esp, NULL);
    g(s, Ipop, &ebp, NULL);
}

/* genasm requires all nodes in 'nl' to map cleanly to operations that are
 * natively supported, as promised in the output of reduce().  No 64-bit
 * operations on x32, no structures, and so on. */
void genasm(Fn *fn)
{
    struct Isel is = {0,};
    int i;

    is.locs = fn->locs;

    prologue(&is, fn->stksz);
    for (i = 0; i < fn->nn; i++)
        isel(&is, fn->nl[i]);
    epilogue(&is);

    if (fn->isglobl)
        fprintf(stdout, ".globl %s\n", fn->name);
    fprintf(stdout, "%s:\n", fn->name);
    for (i = 0; i < is.ni; i++)
        iprintf(stdout, is.il[i]);
}