/*
 * test_asm.c — host harness for the portable Thumb-16 assembler core.
 *
 *   test_asm <src.s> <out.bin>     assemble src, write raw image to out.bin
 *   test_asm <src.s> --hex         assemble, print hex words to stdout
 *   test_asm <src.s> --csub NAME   print a CSUB block
 *
 * Errors go to stderr; exit code 1 on any assembler error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asm_thumb.h"

static asm_ctx ctx;   /* large; keep off the stack */

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: %s src.s (out.bin | --hex | --csub NAME)\n",argv[0]); return 2; }
    FILE*f=fopen(argv[1],"rb");
    if(!f){ perror(argv[1]); return 2; }
    static char src[200000];
    size_t n=fread(src,1,sizeof(src)-1,f); src[n]=0; fclose(f);

    int rc=asm_assemble(&ctx,src);
    for(int i=0;i<ctx.nerr;i++)
        fprintf(stderr,"line %d: %s\n",ctx.err[i].line,ctx.err[i].msg);
    if(rc!=0) return 1;

    if(strcmp(argv[2],"--hex")==0){
        for(uint32_t i=0;i<ctx.code_len;i+=2){
            unsigned hw=ctx.code[i]|(i+1<ctx.code_len?ctx.code[i+1]<<8:0);
            printf("%04X ",hw);
            if((i&15)==14) printf("\n");
        }
        printf("\n(entry=+%u, %u bytes, %d symbols)\n",ctx.entry,ctx.code_len,ctx.nsym);
        return 0;
    }
    if(strcmp(argv[2],"--csub")==0){
        static char out[300000];
        int k=asm_emit_csub(&ctx, argc>3?argv[3]:"code", "integer, integer, integer, integer", out, sizeof out);
        if(k<0){ fprintf(stderr,"csub buffer too small\n"); return 1; }
        fputs(out,stdout);
        return 0;
    }
    if(strcmp(argv[2],"--elf")==0){
        static uint8_t out[300000];
        int k=asm_emit_elf(&ctx, argc>4?argv[4]:0, out, sizeof out);
        if(k<0){ fprintf(stderr,"elf buffer too small\n"); return 1; }
        const char*path = argc>3?argv[3]:"a.o";
        FILE*o=fopen(path,"wb"); if(!o){ perror(path); return 2; }
        fwrite(out,1,k,o); fclose(o);
        return 0;
    }
    FILE*o=fopen(argv[2],"wb");
    if(!o){ perror(argv[2]); return 2; }
    fwrite(ctx.code,1,ctx.code_len,o); fclose(o);
    return 0;
}
