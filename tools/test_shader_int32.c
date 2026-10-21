// Generates shaders and executes the production-emitted arithmetic as C++ against a uint64 oracle.
// Build: cc -O2 tools/test_shader_int32.c -lm -o build/shader-int32-tests/generate
#include "../runtime/nvn/maxwell/translate.c"

typedef struct { uint64_t w; int kind; } Case;

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    Case cases[256]; int count = 0;
    for (int flags = 0; flags < 8; flags++)
        cases[count++] = (Case){0x1c00000000000000ull | (uint64_t)(flags & 1) << 52 |
            (uint64_t)((flags >> 1) & 1) << 53 | (uint64_t)(flags >> 2) << 56 | 0xFFFFFFFFull << 20, 0};
    for (int flags = 0; flags < 32; flags++)
        cases[count++] = (Case){0x5cc0000000000000ull | (uint64_t)(flags & 1) << 47 |
            (uint64_t)((flags >> 1) & 1) << 48 | (uint64_t)((flags >> 2) & 7) << 49 | 2ull << 20 | 3ull << 39, 1};
    cases[count++] = (Case){0x5cf8000000000000ull | 2ull << 20 | 3ull << 39, 2};
    cases[count++] = (Case){0x5bf8000000000000ull | 2ull << 20 | 3ull << 39, 3};
    const int offsets[] = {0, 1, -1, 0x7FFFFF, -0x800000};
    for (int wide = 0; wide < 2; wide++)
        for (int store = 0; store < 2; store++)
            for (int off = 0; off < 5; off++)
                cases[count++] = (Case){0xeed0000000000000ull | (uint64_t)store << 51 | 4ull << 48 |
                    (uint64_t)wide << 45 | ((uint64_t)offsets[off] & 0xFFFFFF) << 20, 4};
    maxwell_gmem_base = 0x340000000ull;
    maxwell_gmem_size = 0xD0000000ull;
    maxwell_gmem_map_all = true;
    maxwell_gmem_miss_log = true;
    maxwell_no_int64 = true;
    {   // A leftover wide literal/instruction must fail explicitly, never truncate silently.
        ShaderInfo info = {0}; Ctx ctx = {.info = &info, .glsl = true}; Str out = {0};
        glsl_rewrite(&ctx, &out, "uint value = 3ul;");
        if (ctx.fail || strcmp(out.buf, "uint value = 3u;")) return 9;
        free(out.buf); out = (Str){0};
        glsl_rewrite(&ctx, &out, "uint value = 0x100000000ul;");
        if (!ctx.fail) return 10;
        free(out.buf);
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/math.cpp", argv[1]);
    FILE *math = fopen(path, "w"); if (!math) return 3;
    fputs("#include <cstdint>\n#include <cstdio>\n#include <cstdlib>\nusing uint = uint32_t;\n"
          "uint uaddCarry(uint a,uint b,uint &c) { uint64_t n=uint64_t(a)+b; c=uint(n>>32); return uint(n); }\n", math);
    fputs(glsl_int32_helpers, math);
    fputs("struct Result {uint value, carry, address; bool valid;};\n", math);
    for (int i = 0; i < count; i++) {
        uint64_t w = cases[i].w | 1ull << 8 | 7ull << 16; // r0 <- r1, predicate PT
        cases[i].w = w;
        maxwell_no_int64 = true;
        ShaderInfo info = {0}; ShaderKey key = {.glsl = 1};
        Ctx ctx = {.info = &info, .key = &key, .stage = STAGE_COMPUTE, .glsl = true};
        emit_insn(&ctx, w, 8);
        if (ctx.fail) { fprintf(stderr, "%s\n", info.error); return 4; }
        fprintf(math, "Result test%d(uint r0,uint r1,uint r2,uint r3,uint cc) { uint address=0; bool valid=false;\n", i);
        if (cases[i].kind == 4) {
            // Execute the actual address prefix, before the generated GPU buffer access.
            char *end = strstr(ctx.body.buf, "      uint fw =");
            if (!end) return 5;
            fprintf(math, "%.*s address=ga; valid=ok; }\n", (int)(end - ctx.body.buf), ctx.body.buf);
        } else fputs(ctx.body.buf, math);
        fputs("return {r0,cc,address,valid}; }\n", math);
        free(ctx.body.buf);

        for (int stage = 0; stage < 3; stage++) {
            int st = stage == 2 ? STAGE_COMPUTE : stage;
            uint8_t blob[256] = {0};
            size_t header = st == STAGE_COMPUTE ? 0 : 0x80;
            if (st == STAGE_FRAGMENT) { uint32_t omap = 15; memcpy(blob + 0x78, &omap, 4); }
            uint64_t code[8] = {0, w, 0xe30000000007000full, 0xe2400fffff87000full};
            memcpy(blob + header, code, sizeof code);
            ShaderKey glsl = {.glsl = 1};
            ShaderInfo *s = maxwell_translate(blob, header + sizeof code, st, &glsl);
            if (!s->msl) { fprintf(stderr, "case%d stage%d %s\n", i, st, s->error); return 6; }
            snprintf(path, sizeof path, "%s/case%d.%s", argv[1], i, st == STAGE_COMPUTE ? "comp" : st ? "frag" : "vert");
            FILE *f = fopen(path, "w"); if (!f) return 7;
            fputs(s->msl, f); fclose(f); maxwell_free(s);
            // The flag must have no effect on Metal output.
            ShaderKey msl = {0};
            maxwell_no_int64 = false;
            ShaderInfo *a = maxwell_translate(blob, header + sizeof code, st, &msl);
            maxwell_no_int64 = true;
            ShaderInfo *b = maxwell_translate(blob, header + sizeof code, st, &msl);
            if (!a->msl || !b->msl || strcmp(a->msl, b->msl)) return 8;
            maxwell_free(a); maxwell_free(b);
        }
    }
    fputs("using Fn=Result(*)(uint,uint,uint,uint,uint);\nstruct Case {Fn run; uint64_t w; int kind;};\nCase cases[]={\n", math);
    for (int i = 0; i < count; i++) fprintf(math, "{test%d,0x%llxull,%d},\n", i, (unsigned long long)cases[i].w, cases[i].kind);
    fputs("};\n", math);
    fputs(
        "uint state=0x12345678; uint next(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}\n"
        "uint field(uint64_t w,int p,int n){return uint(w>>p)&((1u<<n)-1u);}\n"
        "int main(){ uint64_t total=0; uint edges[]={0,1,0x7fffffff,0x80000000,0xffffffff,31,32,63,0x3fffffff,0x40000000,3,4,0x0fffffff,0x10000000,0x10000001};\n"
        "for(auto c:cases)for(int i=0;i<4096;i++){uint r[4]={next(),next(),next(),next()};uint cc=next()&1;\n"
        "if(i<225){r[1]=edges[i%15];r[2]=edges[(i/15)%15];r[3]=edges[(i*3)%15];}\n"
        "uint expected=r[0],ec=cc; uint64_t addr=0;bool valid=false;uint64_t w=c.w;\n"
        "if(c.kind==0){uint a=field(w,56,1)?-r[1]:r[1];uint64_t n=uint64_t(a)+field(w,20,32-1)+(field(w,51,1)?0x80000000ull:0ull);"
        "if(field(w,53,1))n+=cc;expected=uint(n);if(field(w,52,1))ec=uint(n>>32)&1;}\n"
        "if(c.kind==1){uint a=field(w,51,1)?-r[1]:r[1],b=field(w,50,1)?-r[2]:r[2],d=field(w,49,1)?-r[3]:r[3];"
        "uint64_t n=uint64_t(a)+b+d+(field(w,48,1)?cc:0);expected=uint(n);if(field(w,47,1))ec=uint(n>>32)&1;}\n"
        "if(c.kind==2)expected=uint(((uint64_t(r[3])<<32)|r[1])>>(r[2]&31));\n"
        "if(c.kind==3)expected=uint((((uint64_t(r[3])<<32)|r[1])<<(r[2]&31))>>32);\n"
        "if(c.kind==4){int64_t off=int64_t(int32_t(field(w,20,24)<<8)>>8);uint64_t a=uint64_t(r[1])|(field(w,45,1)?uint64_t(r[2])<<32:0);"
        "addr=a+uint64_t(off)-0x340000000ull;valid=addr<0xD0000000ull;}\n"
        "auto got=c.run(r[0],r[1],r[2],r[3],cc); if(c.kind==4){if(got.valid!=valid||got.address!=uint(addr))return 1;}"
        "else if(got.value!=expected||got.carry!=ec){std::printf(\"kind %d iteration %d mismatch %x/%x carry %u/%u\\n\",c.kind,i,got.value,expected,got.carry,ec);return 2;}total++;}\n"
        "std::printf(\"%llu arithmetic/address vectors passed\\n\",(unsigned long long)total);}\n", math);
    fclose(math);
    printf("%d instruction cases, %d shaders, Metal output unchanged\n", count, count * 3);
    return 0;
}
