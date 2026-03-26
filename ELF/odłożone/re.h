#include <stddef.h>
#include <string.h>

typedef struct {
    const uint8_t *code;
    uint32_t size;
    uint32_t off;

    // prefiksy
    uint8_t prefix_66;
    uint8_t prefix_67;
    uint8_t prefix_f2;
    uint8_t prefix_f3;

    // REX
    uint8_t rex;
    uint8_t rex_w;
    uint8_t rex_r;
    uint8_t rex_x;
    uint8_t rex_b;

    // opcode
    uint8_t opcode;
    uint8_t opcode2; // dla 0F xx

    // ModRM
    uint8_t has_modrm;
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg;
    uint8_t rm;

    // SIB
    uint8_t has_sib;
    uint8_t sib;
    uint8_t scale;
    uint8_t index;
    uint8_t base;

    // displacement
    int32_t disp;
    uint8_t disp_size;

    // immediate
    uint64_t imm;
    uint8_t imm_size;

} x86_instr;

static uint8_t fetch8(x86_instr *ins) {
    if (ins->off >= ins->size) return 0;
    return ins->code[ins->off++];
}

static void decode_prefixes(x86_instr *ins) {
    for (;;) {
        uint8_t b = fetch8(ins);

        switch (b) {
        case 0x66: ins->prefix_66 = 1; continue;
        case 0x67: ins->prefix_67 = 1; continue;
        case 0xF2: ins->prefix_f2 = 1; continue;
        case 0xF3: ins->prefix_f3 = 1; continue;

        // REX
        default:
            if ((b & 0xF0) == 0x40) {
                ins->rex = b;
                ins->rex_w = (b >> 3) & 1;
                ins->rex_r = (b >> 2) & 1;
                ins->rex_x = (b >> 1) & 1;
                ins->rex_b = (b >> 0) & 1;
                continue;
            }

            // nie prefiks → to opcode
            ins->opcode = b;
            return;
        }
    }
}

static void decode_opcode(x86_instr *ins) {
    if (ins->opcode == 0x0F) {
        ins->opcode2 = fetch8(ins);
    }
}

static void decode_modrm(x86_instr *ins) {
    ins->has_modrm = 1;
    ins->modrm = fetch8(ins);

    uint8_t mod = (ins->modrm >> 6) & 3;
    uint8_t reg = (ins->modrm >> 3) & 7;
    uint8_t rm  = (ins->modrm >> 0) & 7;

    ins->mod = mod;
    ins->reg = reg;
    ins->rm  = rm;

    // REX rozszerza pola reg/rm
    if (ins->rex) {
        ins->reg |= ins->rex_r << 3;
        ins->rm  |= ins->rex_b << 3;
    }

    // --- SIB ---
    // SIB występuje, gdy dolne 3 bity rm == 4 i mod != 3
    if (mod != 3 && (rm & 7) == 4) {
        ins->has_sib = 1;
        ins->sib = fetch8(ins);

        uint8_t scale = (ins->sib >> 6) & 3;
        uint8_t index = (ins->sib >> 3) & 7;
        uint8_t base  = (ins->sib >> 0) & 7;

        ins->scale = scale;
        ins->index = index;
        ins->base  = base;

        if (ins->rex) {
            ins->index |= ins->rex_x << 3;
            ins->base  |= ins->rex_b << 3;
        }
    }

    // --- displacement ---
    if (mod == 1) {
        ins->disp_size = 1;
        ins->disp = (int8_t)fetch8(ins);
    }
    else if (mod == 2) {
        ins->disp_size = 4;
        ins->disp = (int32_t)(
            fetch8(ins) |
            (fetch8(ins) << 8) |
            (fetch8(ins) << 16) |
            (fetch8(ins) << 24)
        );
    }
    else if (mod == 0 && (rm & 7) == 5 && !ins->has_sib) {
        // disp32 przy [rip+disp] lub [disp32]
        ins->disp_size = 4;
        ins->disp = (int32_t)(
            fetch8(ins) |
            (fetch8(ins) << 8) |
            (fetch8(ins) << 16) |
            (fetch8(ins) << 24)
        );
    }
}

static void decode_imm(x86_instr *ins, uint8_t size) {
    ins->imm_size = size;
    ins->imm = 0;

    for (uint8_t i = 0; i < size; i++) {
        ins->imm |= ((uint64_t)fetch8(ins)) << (i * 8);
    }
}

static void decode_instr(x86_instr *ins) {
    decode_prefixes(ins);
    decode_opcode(ins);

    // --- instrukcje z prefiksem 0F ---
    if (ins->opcode == 0x0F) {
        switch (ins->opcode2) {

        // Jcc rel32
        case 0x80 ... 0x8F:
            decode_imm(ins, 4);
            return;

        // CMOVcc r64, r/m64
        case 0x40 ... 0x4F:
            decode_modrm(ins);
            return;

        // SETcc r/m8
        case 0x90 ... 0x9F:
            decode_modrm(ins);
            return;

        // MOVZX / MOVSX
        case 0xB6: // movzx r64, r/m8
        case 0xB7: // movzx r64, r/m16
        case 0xBE: // movsx r64, r/m8
        case 0xBF: // movsx r64, r/m16
            decode_modrm(ins);
            return;

        default:
            // nieznane 0F xx → spróbuj ModRM (często i tak jest)
            decode_modrm(ins);
            return;
        }
    }

    // --- instrukcje 1‑bajtowe ---
    switch (ins->opcode) {

    // mov r64, imm32
    case 0xB8 ... 0xBF:
        ins->reg = (ins->opcode - 0xB8);
        if (ins->rex) ins->reg |= ins->rex_b << 3;
        decode_imm(ins, 4);
        return;

    // mov r/m8, r8 / mov r8, r/m8
    case 0x88:
    case 0x8A:
        decode_modrm(ins);
        return;

    // mov r/m64, r64 / mov r64, r/m64
    case 0x89:
    case 0x8B:
        decode_modrm(ins);
        return;

    // pop r/m64 (8F /0)
    case 0x8F:
        decode_modrm(ins);
        return;

    // lea r64, r/m64
    case 0x8D:
        decode_modrm(ins);
        return;

    // call/jmp rel32
    case 0xE8:
    case 0xE9:
        decode_imm(ins, 4);
        return;

    // ret
    case 0xC3:
        return;

    // Jcc rel8
    case 0x70 ... 0x7F:
        decode_imm(ins, 1);
        return;

    // JMP rel8
    case 0xEB:
        decode_imm(ins, 1);
        return;

    // push/pop r64
    case 0x50 ... 0x57:
    case 0x58 ... 0x5F:
        return;

    // ALU r/m, r / r, r/m
    case 0x01: case 0x03:
    case 0x29: case 0x2B:
    case 0x39: case 0x3B:
    case 0x21: case 0x23:
    case 0x09: case 0x0B:
    case 0x31: case 0x33:
    case 0x85:
        decode_modrm(ins);
        return;

    // mov r/m64, imm32 (C7 /0)
    case 0xC7:
        decode_modrm(ins);
        decode_imm(ins, 4);
        return;

    // grupa 1: r/m64, imm32
    case 0x81:
        decode_modrm(ins);
        decode_imm(ins, 4);
        return;

    // grupa 1: r/m64, imm8
    case 0x83:
        decode_modrm(ins);
        decode_imm(ins, 1);
        return;

    // grupa 3: r/m8
    case 0xF6:
        decode_modrm(ins);
        if ((ins->reg & 7) == 0) // TEST r/m8, imm8
            decode_imm(ins, 1);
        return;

    // grupa 3: r/m64
    case 0xF7:
        decode_modrm(ins);
        if ((ins->reg & 7) == 0) // TEST r/m64, imm32
            decode_imm(ins, 4);
        return;

    // shifty/roty
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3:
        decode_modrm(ins);
        return;

    // mov r/m8, imm8 (C6 /0)
    case 0xC6:
        decode_modrm(ins);
        decode_imm(ins, 1);
        return;

    // grupa 5: inc/dec/call/jmp/push r/m64
    case 0xFF:
        decode_modrm(ins);
        return;

    default:
        // fallback: spróbuj ModRM – wiele instrukcji tu wpadnie
        decode_modrm(ins);
        return;
    }
}

static const char* reg8_name(uint8_t r) {
    static const char* names[16] = {
        "al","cl","dl","bl","spl","bpl","sil","dil",
        "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"
    };
    return (r < 16) ? names[r] : "r?b";
}


static const char* reg64_name(uint8_t r) {
    static const char* names[16] = {
        "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"
    };
    return (r < 16) ? names[r] : "r?";
}

static void print_disp(int32_t disp) {
    if (disp == 0) return;

    if (disp < 0) {
        fb_puts("-");
        fb_put_hex64((uint64_t)(-disp));
    } else {
        fb_puts("+");
        fb_put_hex64((uint64_t)disp);
    }
}

static void print_mem_operand(const x86_instr *ins) {
    fb_puts("[");

    // 1. RIP-relative
    if (ins->mod == 0 && (ins->rm & 7) == 5 && !ins->has_sib) {
        fb_puts("rip");
        print_disp(ins->disp);
        fb_puts("]");
        return;
    }

    // 2. SIB
    if (ins->has_sib) {
        uint8_t base  = ins->base;
        uint8_t index = ins->index;
        uint8_t scale = ins->scale;

        if (ins->mod == 0 && (base & 7) == 5) {
            fb_put_hex64((uint64_t)ins->disp);
            fb_puts("]");
            return;
        } else {
            fb_puts(reg64_name(base));
        }

        if ((index & 7) != 4) {
            fb_puts("+");
            fb_puts(reg64_name(index));
            if (scale != 0) {
                fb_puts("*");
                fb_put_hex64(1ULL << scale);
            }
        }

        print_disp(ins->disp);
        fb_puts("]");
        return;
    }

    // 3. Bez SIB
    uint8_t rm = ins->rm;

    if (ins->mod == 0 && (rm & 7) == 5) {
        fb_put_hex64((uint64_t)ins->disp);
        fb_puts("]");
        return;
    }

    fb_puts(reg64_name(rm));
    print_disp(ins->disp);
    fb_puts("]");
}

static void print_instr_0F(const x86_instr *ins, uint64_t rip_base) {

    // --- Jcc rel32 ---
    if (ins->opcode2 >= 0x80 && ins->opcode2 <= 0x8F) {
        static const char* cc[16] = {
            "jo","jno","jb","jnb","jz","jnz","jbe","jnbe",
            "js","jns","jp","jnp","jl","jnl","jle","jnle"
        };

        uint8_t cond = ins->opcode2 - 0x80;
        int32_t rel = (int32_t)ins->imm;
        uint64_t target = rip_base + ins->off + rel;

        fb_puts(cc[cond]);
        fb_puts(" 0x");
        fb_put_hex64(target);
        fb_puts_ln("");
        return;
    }

    // --- CMOVcc ---
    if (ins->opcode2 >= 0x40 && ins->opcode2 <= 0x4F) {
        static const char* cc[16] = {
            "o","no","b","nb","z","nz","be","nbe",
            "s","ns","p","np","l","nl","le","nle"
        };

        uint8_t cond = ins->opcode2 - 0x40;

        fb_puts("cmov");
        fb_puts(cc[cond]);
        fb_puts(" ");
        fb_puts(reg64_name(ins->reg));
        fb_puts(", ");

        if (ins->mod == 3)
            fb_puts(reg64_name(ins->rm));
        else
            print_mem_operand(ins);

        fb_puts_ln("");
        return;
    }

    // --- SETcc ---
    if (ins->opcode2 >= 0x90 && ins->opcode2 <= 0x9F) {
        static const char* cc[16] = {
            "o","no","b","nb","z","nz","be","nbe",
            "s","ns","p","np","l","nl","le","nle"
        };

        uint8_t cond = ins->opcode2 - 0x90;

        fb_puts("set");
        fb_puts(cc[cond]);
        fb_puts(" ");

        if (ins->mod == 3)
            fb_puts(reg8_name(ins->rm));
        else
            print_mem_operand(ins);

        fb_puts_ln("");
        return;
    }

    // --- MOVZX / MOVSX ---
    switch (ins->opcode2) {

    case 0xB6: // movzx r64, r/m8
        fb_puts("movzx ");
        fb_puts(reg64_name(ins->reg));
        fb_puts(", ");
        if (ins->mod == 3)
            fb_puts(reg8_name(ins->rm));
        else
            print_mem_operand(ins);
        fb_puts_ln("");
        return;

    case 0xB7: // movzx r64, r/m16
        fb_puts("movzx ");
        fb_puts(reg64_name(ins->reg));
        fb_puts(", ");
        print_mem_operand(ins);
        fb_puts_ln("");
        return;

    case 0xBE: // movsx r64, r/m8
        fb_puts("movsx ");
        fb_puts(reg64_name(ins->reg));
        fb_puts(", ");
        if (ins->mod == 3)
            fb_puts(reg8_name(ins->rm));
        else
            print_mem_operand(ins);
        fb_puts_ln("");
        return;

    case 0xBF: // movsx r64, r/m16
        fb_puts("movsx ");
        fb_puts(reg64_name(ins->reg));
        fb_puts(", ");
        print_mem_operand(ins);
        fb_puts_ln("");
        return;
    }

    // --- Nieznane 0F xx ---
    fb_puts("db 0x0F, 0x");
    fb_put_hex32(ins->opcode2);
    fb_puts_ln("");
}


static void print_instr(const x86_instr *ins, uint64_t rip_base, uint32_t start_off) {
    fb_put_hex64(rip_base + start_off);
    fb_puts(": ");

    // --- instrukcje 0F xx ---
    if (ins->opcode == 0x0F) {
        print_instr_0F(ins, rip_base);
        return;
    }

    switch (ins->opcode) {

    case 0xB8 ... 0xBF: { // mov r64, imm32
        fb_puts("mov ");
        fb_puts(reg64_name(ins->reg));
        fb_puts(", 0x");
        fb_put_hex64(ins->imm);
        fb_puts_ln("");
        break;
    }

    case 0x88: { // mov r/m8, r8
        fb_puts("mov ");
        if (ins->mod == 3)
            fb_puts(reg8_name(ins->rm));
        else
            print_mem_operand(ins);
        fb_puts(", ");
        fb_puts(reg8_name(ins->reg));
        fb_puts_ln("");
        break;
    }

    case 0x8A: { // mov r8, r/m8
        fb_puts("mov ");
        fb_puts(reg8_name(ins->reg));
        fb_puts(", ");
        if (ins->mod == 3)
            fb_puts(reg8_name(ins->rm));
        else
            print_mem_operand(ins);
        fb_puts_ln("");
        break;
    }

    case 0x89: { // mov r/m64, r64
        fb_puts("mov ");
        if (ins->mod == 3)
            fb_puts(reg64_name(ins->rm));
        else
            print_mem_operand(ins);
        fb_puts(", ");
        fb_puts(reg64_name(ins->reg));
        fb_puts_ln("");
        break;
    }

    case 0x8B: { // mov r64, r/m64
        fb_puts("mov ");
        fb_puts(reg64_name(ins->reg));
        fb_puts(", ");
        if (ins->mod == 3)
            fb_puts(reg64_name(ins->rm));
        else
            print_mem_operand(ins);
        fb_puts_ln("");
        break;
    }

    case 0x8F: { // pop r/m64
        if ((ins->reg & 7) != 0) {
            fb_puts("db 0x8F ; invalid /reg\n");
            break;
        }
        fb_puts("pop ");
        if (ins->mod == 3)
            fb_puts(reg64_name(ins->rm));
        else
            print_mem_operand(ins);
        fb_puts_ln("");
        break;
    }

    case 0x8D: { // lea
        fb_puts("lea ");
        fb_puts(reg64_name(ins->reg));
        fb_puts(", ");
        print_mem_operand(ins);
        fb_puts_ln("");
        break;
    }

    case 0xE8: { // call rel32
        int32_t rel = (int32_t)ins->imm;
        uint64_t target = rip_base + ins->off + rel;
        fb_puts("call 0x");
        fb_put_hex64(target);
        fb_puts_ln("");
        break;
    }

    case 0xE9: { // jmp rel32
        int32_t rel = (int32_t)ins->imm;
        uint64_t target = rip_base + ins->off + rel;
        fb_puts("jmp 0x");
        fb_put_hex64(target);
        fb_puts_ln("");
        break;
    }

    case 0xC3:
        fb_puts_ln("ret");
        break;

    case 0x70 ... 0x7F: { // Jcc rel8
        static const char* cc[16] = {
            "jo","jno","jb","jnb","jz","jnz","jbe","jnbe",
            "js","jns","jp","jnp","jl","jnl","jle","jnle"
        };
        uint8_t cond = ins->opcode - 0x70;
        int8_t rel = (int8_t)ins->imm;
        uint64_t target = rip_base + ins->off + rel;
        fb_puts(cc[cond]);
        fb_puts(" 0x");
        fb_put_hex64(target);
        fb_puts_ln("");
        break;
    }

    case 0xEB: { // jmp short
        int8_t rel = (int8_t)ins->imm;
        uint64_t target = rip_base + ins->off + rel;
        fb_puts("jmp 0x");
        fb_put_hex64(target);
        fb_puts_ln("");
        break;
    }

    case 0x50 ... 0x57: { // push r64
        uint8_t r = ins->opcode - 0x50;
        if (ins->rex) r |= ins->rex_b << 3;
        fb_puts("push ");
        fb_puts(reg64_name(r));
        fb_puts_ln("");
        break;
    }

    case 0x58 ... 0x5F: { // pop r64
        uint8_t r = ins->opcode - 0x58;
        if (ins->rex) r |= ins->rex_b << 3;
        fb_puts("pop ");
        fb_puts(reg64_name(r));
        fb_puts_ln("");
        break;
    }

    // --- ALU rm,r / r,rm ---
    case 0x01: fb_puts("add "); goto rm_r;
    case 0x03: fb_puts("add "); goto r_rm;
    case 0x29: fb_puts("sub "); goto rm_r;
    case 0x2B: fb_puts("sub "); goto r_rm;
    case 0x39: fb_puts("cmp "); goto rm_r;
    case 0x3B: fb_puts("cmp "); goto r_rm;
    case 0x21: fb_puts("and "); goto rm_r;
    case 0x23: fb_puts("and "); goto r_rm;
    case 0x09: fb_puts("or "); goto rm_r;
    case 0x0B: fb_puts("or "); goto r_rm;
    case 0x31: fb_puts("xor "); goto rm_r;
    case 0x33: fb_puts("xor "); goto r_rm;
    case 0x85: fb_puts("test "); goto rm_r;

rm_r:
        print_mem_operand(ins);
        fb_puts(", ");
        fb_puts(reg64_name(ins->reg));
        fb_puts_ln("");
        break;

r_rm:
        fb_puts(reg64_name(ins->reg));
        fb_puts(", ");
        print_mem_operand(ins);
        fb_puts_ln("");
        break;

    case 0xC7: { // mov r/m64, imm32
        if ((ins->reg & 7) != 0) {
            fb_puts("db 0xC7 ; invalid /reg\n");
            break;
        }
        fb_puts("mov ");
        print_mem_operand(ins);
        fb_puts(", 0x");
        fb_put_hex64(ins->imm);
        fb_puts_ln("");
        break;
    }

    case 0x81:
    case 0x83: {
        static const char* grp1[8] = {
            "add","or","adc","sbb","and","sub","xor","cmp"
        };
        fb_puts(grp1[ins->reg & 7]);
        fb_puts(" ");
        print_mem_operand(ins);
        fb_puts(", ");
        if (ins->imm_size == 1)
            fb_put_hex64((int8_t)ins->imm);
        else
            fb_put_hex64(ins->imm);
        fb_puts_ln("");
        break;
    }

    case 0xF6:
    case 0xF7: {
        uint8_t grp = ins->reg & 7;
        static const char* names[8] = {
            "test","?","not","neg","mul","imul","div","idiv"
        };
        fb_puts(names[grp]);
        fb_puts(" ");
        print_mem_operand(ins);
        if (grp == 0) {
            fb_puts(", 0x");
            fb_put_hex64(ins->imm);
        }
        fb_puts_ln("");
        break;
    }

    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3: {
        static const char* shift_ops[8] = {
            "rol","ror","rcl","rcr","shl","shr","?","sar"
        };
        fb_puts(shift_ops[ins->reg & 7]);
        fb_puts(" ");
        print_mem_operand(ins);
        fb_puts(", ");
        if (ins->opcode == 0xD0 || ins->opcode == 0xD1)
            fb_puts("1");
        else
            fb_puts("cl");
        fb_puts_ln("");
        break;
    }

    case 0xC6: { // mov r/m8, imm8
        if ((ins->reg & 7) != 0) {
            fb_puts("db 0xC6 ; invalid /reg\n");
            break;
        }
        fb_puts("mov ");
        print_mem_operand(ins);
        fb_puts(", 0x");
        fb_put_hex64(ins->imm);
        fb_puts_ln("");
        break;
    }

    case 0xFF: {
        static const char* grp5[8] = {
            "inc","dec","call","?","jmp","?","push","?"
        };
        fb_puts(grp5[ins->reg & 7]);
        fb_puts(" ");
        print_mem_operand(ins);
        fb_puts_ln("");
        break;
    }

    default:
        fb_puts("db 0x");
        fb_put_hex32(ins->opcode);
        fb_puts_ln("");
        break;
    }
}

static void decode_buffer(const uint8_t *code, uint32_t size, uint64_t rip_base) {
    x86_instr ins = {0};
    ins.code = code;
    ins.size = size;

    while (ins.off < size) {
        uint32_t start = ins.off;
        memset(&ins.prefix_66, 0, sizeof(x86_instr) - offsetof(x86_instr, prefix_66));

        decode_instr(&ins);
        print_instr(&ins, rip_base, start);
    }
}

static void re_decompile(const re_binary *bin) {
    fb_puts_ln("=== raw x86-64 decode ===");
    decode_buffer(bin->data, bin->size, 0); // na razie rip_base = 0
}