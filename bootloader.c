#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include <stddef.h>
#define BIN_BUF_SIZE (8 * 1024 * 1024) // 8 MB na surową binarkę
#define EFI_SCAN_ENTER     0x17
#define EFI_SCAN_BACKSPACE 0x08

// ================== LEXER COŚ DO TEGO ===========================

#define MAX_TOKENS 8
#define MAX_TOKEN_LEN 64

// ===================== DEFINICJA 'PARSED CMD LEX' ========================

typedef struct {
    char tokens[MAX_TOKENS][MAX_TOKEN_LEN];
    int count;
} parsed_cmd;

// ===================== IMAGE HANDE POINTER ====================================

static EFI_HANDLE IH;

// ===================== GOP GLOBALS =====================

static EFI_GRAPHICS_OUTPUT_PROTOCOL* g_gop = NULL;
static uint32_t* g_fb = NULL;
static uint32_t g_fb_width = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_pitch = 0;

// ================= EKSPORT SYMBOLI DO FRAMEBUFFERA ========================

typedef struct {
    uint32_t* fb;
    uint32_t  fb_width;
    uint32_t  fb_height;
    uint32_t  fb_pitch;
} BOOT_EXPORTS;

static BOOT_EXPORTS g_exports;

// ================= INNE GLOBALE ======================

static EFI_FILE_PROTOCOL* g_root = NULL;
static EFI_FILE_PROTOCOL* g_cwd  = NULL;

static UINTN g_elf_entry = 0;
static UINTN g_elf_base  = 0;

// ================ LEXER =================

parsed_cmd lex(const char *input) {
    parsed_cmd out = {0};
    int i = 0; // index w input
    int t = 0; // numer tokena
    int c = 0; // index w tokenie

    // 1. pomiń początkowe spacje i taby
    while (input[i] != '\0' && (input[i] == ' ' || input[i] == '\t'))
        i++;

    // 2. główna pętla
    while (input[i] != '\0' && t < MAX_TOKENS) {

        // separator tokenów
        if (input[i] == ' ' || input[i] == '\t') {

            // zakończ token jeśli coś w nim jest
            if (c > 0) {
                out.tokens[t][c] = '\0';
                t++;
                c = 0;
            }

            // pomiń kolejne spacje/taby
            while (input[i] != '\0' && (input[i] == ' ' || input[i] == '\t'))
                i++;

            continue;
        }

        // normalny znak
        if (c < MAX_TOKEN_LEN - 1) {
            out.tokens[t][c++] = input[i++];
        } else {
            // token za długi — obetnij, ale NIE dopisuj poza bufor
            i++;
        }
    }

    // 3. zakończ ostatni token
    if (c > 0 && t < MAX_TOKENS) {
        out.tokens[t][c] = '\0';
        t++;
    }

    out.count = t;
    return out;
}

// ===================== EXIT BOOT SERVICES ===========================

static EFI_STATUS do_exit_boot_services(EFI_HANDLE ImageHandle) {
    EFI_STATUS Status;

    UINTN MemMapSize = 0;
    EFI_MEMORY_DESCRIPTOR* MemMap = NULL;
    UINTN MapKey;
    UINTN DescSize;
    UINT32 DescVersion;

    //
    // 1. Pierwsze GetMemoryMap – tylko po rozmiar
    //
    Status = uefi_call_wrapper(
        ST->BootServices->GetMemoryMap,
        5,
        &MemMapSize,
        MemMap,
        &MapKey,
        &DescSize,
        &DescVersion
    );

    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"[ERR] GetMemoryMap (probe) failed: %r\r\n", Status);
        return Status;
    }

    // dorzuć zapas
    MemMapSize += DescSize * 8;

    Status = uefi_call_wrapper(
        ST->BootServices->AllocatePool,
        3,
        EfiLoaderData,
        MemMapSize,
        (void**)&MemMap
    );
    if (EFI_ERROR(Status)) {
        Print(L"[ERR] AllocatePool failed: %r\r\n", Status);
        return Status;
    }

    //
    // 2. Właściwe GetMemoryMap
    //
    Status = uefi_call_wrapper(
        ST->BootServices->GetMemoryMap,
        5,
        &MemMapSize,
        MemMap,
        &MapKey,
        &DescSize,
        &DescVersion
    );
    if (EFI_ERROR(Status)) {
        Print(L"[ERR] GetMemoryMap failed: %r\r\n", Status);
        return Status;
    }

    //
    // 3. ExitBootServices
    //
    Status = uefi_call_wrapper(
        ST->BootServices->ExitBootServices,
        2,
        ImageHandle,
        MapKey
    );

    return Status;
}

// ================ TRIPLE FAULT ====================

struct IDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct IDTR old_idtr;
static struct IDTR empty_idtr = {
    .limit = 0,
    .base  = 0,
};

static inline void save_idt(void) {
    __asm__ volatile ("sidt %0" : "=m"(old_idtr));
}

static inline void load_empty_idt(void) {
    __asm__ volatile ("lidt %0" : : "m"(empty_idtr));
}

void trigger_triple_fault(void) {
    EFI_STATUS Status;

    // --- WYJŚCIE Z BOOT SERVICES ---
    // użyj globalnego IH albo g_image_handle – ważne, żeby to był ten sam handle,
    // którego dostałeś w efi_main
    Status = do_exit_boot_services(IH);
    if (EFI_ERROR(Status)) {
        Print(L"[BOOT] ExitBootServices nie powiodl sie: %r\r\n", Status);
        return;    // nie możemy zwrócić EFI_STATUS z funkcji typu void
    }

    // od tego momentu NIE używamy już BootServices

    __asm__ volatile ("cli");      // wyłącz przerwania maskowalne
    // save_idt();                 // możesz zostawić lub wywalić, i tak nie wrócisz
    load_empty_idt();              // załadowanie pustego IDT

    __asm__ volatile ("ud2");      // nielegalna instrukcja → #UD → #DF → triple fault → reset

    for (;;)
        ;                          // jeśli z jakiegoś powodu nie zresetuje
}

// ================ STRCMP ================

static int my_strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

// ========== NIE WIEM PO CO ===============

static EFI_SYSTEM_TABLE* g_st = NULL;

static void* g_bin_buf = NULL;
static UINTN g_bin_size = 0;

static EFI_HANDLE g_image_handle = NULL;

// ================= CLS ==================

static void cls(void) {
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
}

// =============== INPUT ==================

static void read_line(char* buf, UINTN max_len) {
    UINTN pos = 0;

    while (1) {
        EFI_INPUT_KEY key;
        UINTN idx;

        uefi_call_wrapper(g_st->BootServices->WaitForEvent,
                          3, 1, &g_st->ConIn->WaitForKey, &idx);

        if (EFI_ERROR(uefi_call_wrapper(g_st->ConIn->ReadKeyStroke,
                                        2, g_st->ConIn, &key))) {
            continue;
        }

        CHAR16 c = key.UnicodeChar;

        if (c == CHAR_CARRIAGE_RETURN || key.ScanCode == EFI_SCAN_ENTER) {
            Print(L"\r\n");
            buf[pos] = '\0';
            return;
        }

        if (c == CHAR_BACKSPACE || key.ScanCode == EFI_SCAN_BACKSPACE) {
            if (pos > 0) {
                pos--;
                Print(L"\b \b");
            }
            continue;
        }

        if (c >= 32 && c < 127) {
            if (pos + 1 < max_len) {
                buf[pos++] = (char)c;
                CHAR16 out[2] = { c, 0 };
                Print(out);
            }
        }
    }
}

// ====================== URUCHAMIANIE BINAREK ========================

// PARSER HEXA I SKOK

static UINTN parse_hex(const char* s) {
    UINTN value = 0;
    while (*s) {
        char c = *s++;
        UINTN digit;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F')
            digit = 10 + (c - 'A');
        else
            break;

        value = (value << 4) | digit;
    }
    return value;
}

// KOMENDA 'RUN'

typedef void (*raw_entry_t)(BOOT_EXPORTS*);

static void cmd_run(const char* arg) {
    if (arg == NULL || *arg == '\0') {
        Print(L"Uzycie: run <adres_hex>\r\n\r\n");
        return;
    }

    UINTN addr = parse_hex(arg);
    if (addr == 0) {
        Print(L"[ERR] run: niepoprawny adres '%a'\r\n\r\n", arg);
        return;
    }

    Print(L"[RUN] skaczemy pod 0x%lx\r\n", (UINT64)addr);

    raw_entry_t entry = (raw_entry_t)(UINTN)addr;

    // UWAGA: od tego momentu to kod binarki przejmuje sterowanie
    entry(&g_exports);

    Print(L"[RUN] powrot z binarki (nie powinno sie zdarzyc)\r\n\r\n");
}

// ====================== BUFOR NA BINARKE ==========================

static EFI_STATUS ensure_bin_buffer(void) {
    if (g_bin_buf != NULL)
        return EFI_SUCCESS;

    EFI_STATUS ST = uefi_call_wrapper(
        g_st->BootServices->AllocatePool,
        3, EfiLoaderData, BIN_BUF_SIZE, &g_bin_buf
    );

    if (EFI_ERROR(ST)) {
        Print(L"[ERR] AllocatePool for bin buffer failed: %r\r\n", ST);
        return ST;
    }

    g_bin_size = 0;
    return EFI_SUCCESS;
}

// ===================== STAŁE ELF ========================

#define EI_NIDENT 16

#define EI_CLASS 4
#define EI_DATA 5

// e_ident[EI_CLASS]
#define ELFCLASS32 1
#define ELFCLASS64 2

// e_ident[EI_DATA]
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// e_type
#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2
#define ET_DYN    3
#define ET_CORE   4

// e_machine
#define EM_X86_64 62

// p_type
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7

// ================= ELF64 STRUCTS =================

typedef struct {
    unsigned char e_ident[EI_NIDENT]; // magic + info
    uint16_t      e_type;             // typ pliku (ET_EXEC, ET_DYN, ...)
    uint16_t      e_machine;          // architektura (EM_X86_64)
    uint32_t      e_version;          // wersja (zwykle 1)
    uint64_t      e_entry;            // adres entry pointu
    uint64_t      e_phoff;            // offset do tablicy program headers
    uint64_t      e_shoff;            // offset do tablicy section headers
    uint32_t      e_flags;            // flagi (dla x86_64 zwykle 0)
    uint16_t      e_ehsize;           // rozmiar tego nagłówka
    uint16_t      e_phentsize;        // rozmiar pojedynczego program headera
    uint16_t      e_phnum;            // liczba program headers
    uint16_t      e_shentsize;        // rozmiar pojedynczego section headera
    uint16_t      e_shnum;            // liczba section headers
    uint16_t      e_shstrndx;         // indeks sekcji z nazwami sekcji
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;   // typ segmentu (PT_LOAD, PT_DYNAMIC, ...)
    uint32_t p_flags;  // flagi (R/W/X)
    uint64_t p_offset; // offset w pliku
    uint64_t p_vaddr;  // adres wirtualny w pamięci
    uint64_t p_paddr;  // adres fizyczny (zwykle ignorowany)
    uint64_t p_filesz; // rozmiar danych w pliku
    uint64_t p_memsz;  // rozmiar w pamięci (>= filesz, BSS)
    uint64_t p_align;  // alignment (np. 0x1000)
} Elf64_Phdr;

// ====================== DEFINICJE MAKR I STRUKTUR ELF64 ===========================

typedef struct {
    int64_t  d_tag;
    uint64_t d_un;
} Elf64_Dyn;

typedef struct {
    uint32_t    st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t    st_shndx;
    uint64_t    st_value;
    uint64_t    st_size;
} Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

#define ELF64_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)  ((uint32_t)(i))

// typy relokacji x86_64
#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_32         10
#define R_X86_64_32S        11

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_INIT     12
#define DT_FINI     13
#define DT_SONAME   14
#define DT_RPATH    15
#define DT_SYMBOLIC 16
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_PLTREL   20
#define DT_DEBUG    21
#define DT_TEXTREL  22
#define DT_JMPREL   23

// ====================== VADDR --> RUNTIME? ALBO PTR (NIE WIEM JAK TO ZAPISAĆ W KOMENTARZU)

static inline void* elf_vaddr_to_ptr(UINT8* base8, UINT64 min_vaddr, UINT64 vaddr)
{
    return base8 + (vaddr - min_vaddr);
}

 // ===================== Funkcja pomocnicza: lookup symbolu w tym samym ELF-ie =========================

        static UINT64 get_symbol_value_c(Elf64_Sym* dynsym, UINT32 sym_index, UINT8* dyn_base, UINT64 min_vaddr)
        {
            Elf64_Sym* s = &dynsym[sym_index];
            return (UINT64)(UINTN)(dyn_base + (s->st_value - min_vaddr));
        }

// ====================== ELF LOADER =======================

static UINTN load_elf_advanced(void* buf, UINTN size, UINTN* out_entry, UINTN* out_base)
{
    if (out_entry) *out_entry = 0;
    if (out_base)  *out_base  = 0;

    if (size < sizeof(Elf64_Ehdr)) {
        Print(L"[ELF] Blad: plik za maly (size=%lu)\r\n", (UINT64)size);
        return 0;
    }

    Elf64_Ehdr* eh = (Elf64_Ehdr*)buf;

    // --- Walidacja nagłówka ---
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        Print(L"[ELF] Blad: niepoprawny magic\r\n");
        return 0;
    }

    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        Print(L"[ELF] Blad: nie ELF64\r\n");
        return 0;
    }

    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        Print(L"[ELF] Blad: nie little-endian ELF\r\n");
        return 0;
    }

    if (eh->e_machine != EM_X86_64) {
        Print(L"[ELF] Blad: nie ELF64 x86_64 (e_machine=%u)\r\n", eh->e_machine);
        return 0;
    }

    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        Print(L"[ELF] Blad: brak program headers\r\n");
        return 0;
    }

    UINT64 ph_end = eh->e_phoff + (UINT64)eh->e_phnum * sizeof(Elf64_Phdr);
    if (ph_end > size) {
        Print(L"[ELF] Blad: tabela program headers wychodzi poza plik\r\n");
        return 0;
    }

    Elf64_Phdr* ph = (Elf64_Phdr*)((UINT8*)buf + eh->e_phoff);

    Print(L"[ELF] Naglowek OK. e_entry=0x%lx, e_phnum=%u\r\n",
          eh->e_entry, eh->e_phnum);

    // --- Wyznacz zakres segmentów PT_LOAD ---
    UINT64 min_vaddr = (UINT64)-1;
    UINT64 max_vaddr = 0;
    BOOLEAN any_load = FALSE;

    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        any_load = TRUE;

        if (ph[i].p_memsz == 0)
            continue;

        UINT64 start = ph[i].p_vaddr;
        UINT64 end   = ph[i].p_vaddr + ph[i].p_memsz;

        if (start < min_vaddr) min_vaddr = start;
        if (end   > max_vaddr) max_vaddr = end;
    }

    if (!any_load) {
        Print(L"[ELF] Blad: brak segmentow PT_LOAD\r\n");
        return 0;
    }

    UINT64 image_size = max_vaddr - min_vaddr;

    Print(L"[ELF] Zakres PT_LOAD: [0x%lx .. 0x%lx), size=0x%lx\r\n",
          min_vaddr, max_vaddr, image_size);

    // --- Alokacja pamięci ---
    EFI_STATUS st;
    void* image_base = NULL;

    st = uefi_call_wrapper(
        g_st->BootServices->AllocatePool,
        3,
        EfiLoaderData,
        (UINTN)image_size,
        &image_base
    );

    if (EFI_ERROR(st) || image_base == NULL) {
        Print(L"[ELF] Blad: AllocatePool(%lu) nie powiodl sie: %r\r\n",
              (UINT64)image_size, st);
        return 0;
    }

    Print(L"[ELF] Zaalokowano obraz pod 0x%lx (size=0x%lx)\r\n",
          (UINT64)(UINTN)image_base, image_size);

    // Wyzeruj całość
    UINT8* base8 = (UINT8*)image_base;
    for (UINT64 i = 0; i < image_size; i++)
        base8[i] = 0;

    // --- Ładowanie segmentów ---
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        if (ph[i].p_memsz == 0)
            continue;

        UINT64 seg_start = ph[i].p_vaddr;
        UINT64 seg_offset = seg_start - min_vaddr;

        UINT8* dst = base8 + seg_offset;
        UINT8* src = (UINT8*)buf + ph[i].p_offset;

        UINT64 end_in_file = ph[i].p_offset + ph[i].p_filesz;
        if (end_in_file > size) {
            Print(L"[ELF] Blad: segment %u wychodzi poza plik\r\n", i);
            return 0;
        }

        Print(L"[ELF] Segment %u: vaddr=0x%lx, filesz=0x%lx, memsz=0x%lx\r\n",
              i, ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_memsz);

        // kopiowanie części z pliku
        for (UINT64 j = 0; j < ph[i].p_filesz; j++)
            dst[j] = src[j];

        // reszta (BSS) już wyzerowana
    }

        // --- Szukamy PT_DYNAMIC ---
    Elf64_Dyn* dyn = NULL;
    UINT64 dyn_size = 0;

    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC)
            continue;

        if (ph[i].p_memsz == 0)
            continue;

        dyn = (Elf64_Dyn*)elf_vaddr_to_ptr(base8, min_vaddr, ph[i].p_vaddr);
        dyn_size = ph[i].p_memsz;
        break;
    }

    if (dyn) {
        Print(L"[ELF] Znaleziono PT_DYNAMIC\r\n");

        // wskaźniki na rzeczy z DT_*
        UINT8*  dyn_base   = base8;
        Elf64_Sym* dynsym  = NULL;
        CHAR8*  dynstr     = NULL;
        UINT64  dynstr_sz  = 0;
        Elf64_Rela* rela   = NULL;
        UINT64  rela_sz    = 0;
        UINT64  rela_ent   = sizeof(Elf64_Rela);
        Elf64_Rela* jmprel = NULL;
        UINT64  jmprel_sz  = 0;
        UINT64  plt_rel_type = 0;

        // parsujemy tablicę dynamiczną
        UINT64 dyn_count = dyn_size / sizeof(Elf64_Dyn);
        for (UINT64 i = 0; i < dyn_count; i++) {
            switch (dyn[i].d_tag) {
            case DT_NULL:
                i = dyn_count; // koniec
                break;
            case DT_SYMTAB:
                dynsym = (Elf64_Sym*)elf_vaddr_to_ptr(dyn_base, min_vaddr, dyn[i].d_un);
                break;
            case DT_STRTAB:
                dynstr = (CHAR8*)elf_vaddr_to_ptr(dyn_base, min_vaddr, dyn[i].d_un);
                break;
            case DT_STRSZ:
                dynstr_sz = dyn[i].d_un;
                break;
            case DT_RELA:
                rela = (Elf64_Rela*)elf_vaddr_to_ptr(dyn_base, min_vaddr, dyn[i].d_un);
                break;
            case DT_RELASZ:
                rela_sz = dyn[i].d_un;
                break;
            case DT_RELAENT:
                rela_ent = dyn[i].d_un;
                break;
            case DT_JMPREL:
                jmprel = (Elf64_Rela*)elf_vaddr_to_ptr(dyn_base, min_vaddr, dyn[i].d_un);
                break;
            case DT_PLTRELSZ:
                jmprel_sz = dyn[i].d_un;
                break;
            case DT_PLTREL:
                plt_rel_type = dyn[i].d_un;
                break;
            default:
                break;
            }
        }

        // --- Relokacje z .rela.dyn ---
        if (rela && rela_sz && rela_ent == sizeof(Elf64_Rela)) {
            UINT64 count = rela_sz / sizeof(Elf64_Rela);
            for (UINT64 i = 0; i < count; i++) {
                Elf64_Rela* r = &rela[i];
                UINT32 type = ELF64_R_TYPE(r->r_info);
                UINT32 sym  = ELF64_R_SYM(r->r_info);
                UINT8*  loc = (UINT8*)elf_vaddr_to_ptr(dyn_base, min_vaddr, r->r_offset);

                UINT64 S = 0;
                UINT64 A = (UINT64)r->r_addend;
                UINT64 P = (UINT64)(UINTN)loc;
                UINT64 B = (UINT64)(UINTN)dyn_base;

                if (type != R_X86_64_RELATIVE && sym != 0)
                    S = get_symbol_value_c(dynsym, sym, dyn_base, min_vaddr);

                switch (type) {
                case R_X86_64_NONE:
                    break;
                case R_X86_64_RELATIVE:
                    *(UINT64*)loc = B + A;
                    break;
                case R_X86_64_64:
                    *(UINT64*)loc = S + A;
                    break;
                case R_X86_64_32:
                    *(UINT32*)loc = (UINT32)(S + A);
                    break;
                case R_X86_64_32S:
                    *(INT32*)loc = (INT32)(S + A);
                    break;
                case R_X86_64_PC32:
                    *(UINT32*)loc = (UINT32)((S + A) - P);
                    break;
                case R_X86_64_GLOB_DAT:
                case R_X86_64_JUMP_SLOT:
                    *(UINT64*)loc = S;
                    break;
                default:
                    Print(L"[ELF] UWAGA: nieobslugiwany typ relokacji: %u\r\n", type);
                    break;
                }
            }
        }

        // --- Relokacje PLT (.rela.plt) ---
        if (jmprel && jmprel_sz && plt_rel_type == DT_RELA) {
            UINT64 count = jmprel_sz / sizeof(Elf64_Rela);
            for (UINT64 i = 0; i < count; i++) {
                Elf64_Rela* r = &jmprel[i];
                UINT32 type = ELF64_R_TYPE(r->r_info);
                UINT32 sym  = ELF64_R_SYM(r->r_info);
                UINT8*  loc = (UINT8*)elf_vaddr_to_ptr(dyn_base, min_vaddr, r->r_offset);

                UINT64 S = 0;
                UINT64 A = (UINT64)r->r_addend;
                UINT64 B = (UINT64)(UINTN)dyn_base;

                if (sym != 0)
                    S = get_symbol_value_c(dynsym, sym, dyn_base, min_vaddr);

                switch (type) {
                case R_X86_64_JUMP_SLOT:
                case R_X86_64_GLOB_DAT:
                    *(UINT64*)loc = S;
                    break;
                case R_X86_64_RELATIVE:
                    *(UINT64*)loc = B + A;
                    break;
                default:
                    Print(L"[ELF] UWAGA: nieobslugiwany typ relokacji PLT: %u\r\n", type);
                    break;
                }
            }
        }

        Print(L"[ELF] Relokacje zastosowane\r\n");
    } else {
        Print(L"[ELF] Brak PT_DYNAMIC - zakladam brak relokacji\r\n");
    }

    // --- Entry point ---
    UINT64 entry_vaddr = eh->e_entry;
    UINT64 entry_offset = entry_vaddr - min_vaddr;
    UINT8* entry_ptr = base8 + entry_offset;

    Print(L"[ELF] Entry vaddr=0x%lx -> ptr=0x%lx\r\n",
          entry_vaddr, (UINT64)(UINTN)entry_ptr);

    if (out_entry) *out_entry = (UINTN)entry_ptr;
    if (out_base)  *out_base  = (UINTN)image_base;

    return (UINTN)entry_ptr;
}

// ==================== LOAD KOMPLET ====================

static void cmd_load(const char* arg) {
    if (arg == NULL || *arg == '\0') {
        Print(L"Uzycie: load <plik.bin>\r\n\r\n");
        return;
    }

    if (EFI_ERROR(ensure_bin_buffer())) {
        Print(L"Nie moge przygotowac bufora na binarke.\r\n\r\n");
        return;
    }

    if (g_cwd == NULL) {
        Print(L"[ERR] load: g_cwd == NULL\r\n\r\n");
        return;
    }

    // ASCII -> UTF-16
    CHAR16 filename[256];
    UINTN i = 0;
    while (arg[i] && i < 255) {
        filename[i] = (CHAR16)arg[i];
        i++;
    }
    filename[i] = 0;

    EFI_FILE_PROTOCOL* file = NULL;
    EFI_STATUS ST = uefi_call_wrapper(
        g_cwd->Open,
        5,
        g_cwd,
        &file,
        filename,
        EFI_FILE_MODE_READ,
        0
    );
    if (EFI_ERROR(ST) || file == NULL) {
        Print(L"[ERR] Nie moge otworzyc pliku: %r\r\n\r\n", ST);
        return;
    }

    UINTN size = BIN_BUF_SIZE;
    ST = uefi_call_wrapper(file->Read, 3, file, &size, g_bin_buf);
    if (EFI_ERROR(ST)) {
        Print(L"[ERR] Read failed: %r\r\n\r\n", ST);
        uefi_call_wrapper(file->Close, 1, file);
        return;
    }

    g_bin_size = size;
    uefi_call_wrapper(file->Close, 1, file);

    Print(L"Zaladowano plik do bufora. Rozmiar: %lu bajtow\r\n\r\n", g_bin_size);
    Print(L"Adres bufora z binarka: 0x%lx\r\n\r\n", (UINT64)(UINTN)g_bin_buf);
}

// ======================= ELF RUN ===========================

static void cmd_runelf(void) {
    // 1. Sprawdź, czy coś jest w buforze
    if (g_bin_buf == NULL || g_bin_size == 0) {
        Print(L"[ELF] Najpierw load <plik.elf>\r\n\r\n");
        return;
    }

    Print(L"[ELF] Proba zaladowania ELF z bufora: addr=0x%lx, size=%lu\r\n",
          (UINT64)(UINTN)g_bin_buf, (UINT64)g_bin_size);

    // 2. Zaawansowany loader
    UINTN entry = 0;
    UINTN base  = 0;

    UINTN res = load_elf_advanced(g_bin_buf, g_bin_size, &entry, &base);
    if (res == 0 || entry == 0) {
        Print(L"[ELF] Nie udalo sie zaladowac ELF - patrz komunikaty powyzej\r\n\r\n");
        return;
    }

    Print(L"[ELF] Obraz ELF baza=0x%lx, entry=0x%lx\r\n",
          (UINT64)base, (UINT64)entry);

    // 3. (Na razie) UWAGA: nadal w Boot Services
    Print(L"[ELF] UWAGA: nadal jestesmy w BootServices (brak ExitBootServices)\r\n");
    Print(L"[ELF] Skok do entry point...\r\n\r\n");

    // 4. Skok do entry
    raw_entry_t entry_fn = (raw_entry_t)(UINTN)entry;
    entry_fn(&g_exports);

    // 5. Jeśli wróci – to już podejrzane
    Print(L"[ELF] Powrot z ELF (nie powinno sie zdarzyc)\r\n\r\n");
}

// ====================== LISTA PLIKÓW ==================================

static void cmd_ls(void) {
    if (g_cwd == NULL) {
        Print(L"[ERR] ls: g_cwd == NULL\r\n\r\n");
        return;
    }

    EFI_STATUS ST;
    EFI_FILE_PROTOCOL* dir = NULL;

    ST = uefi_call_wrapper(
        g_cwd->Open,
        5,
        g_cwd,
        &dir,
        L".",
        EFI_FILE_MODE_READ,
        0
    );
    if (EFI_ERROR(ST) || dir == NULL) {
        Print(L"[ERR] ls: Open(.) failed: %r\r\n\r\n", ST);
        return;
    }

    Print(L"Zawartosc katalogu:\r\n");

    // Stały bufor 4 KB
    UINT8 buffer[4096];
    EFI_FILE_INFO* info = (EFI_FILE_INFO*)buffer;

    while (1) {
        UINTN buf_size = sizeof(buffer);

        ST = uefi_call_wrapper(dir->Read, 3, dir, &buf_size, buffer);

        if (EFI_ERROR(ST)) {
            Print(L"[ERR] ls: Read = %r\r\n", ST);
            break;
        }

        if (buf_size == 0)
            break;

        if (info->FileName[0] == L'\0')
            continue;

        BOOLEAN is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;

        if (is_dir)
            Print(L"<DIR>  %s\r\n", info->FileName);
        else
            Print(L"       %s  (%lu bajtow)\r\n", info->FileName, info->FileSize);
    }

    uefi_call_wrapper(dir->Close, 1, dir);
    Print(L"\r\n");
}

// ====================== ZMIANA FOLDERU ================================

static void cmd_cd(const char* arg) {
    if (arg == NULL || *arg == '\0') {
        Print(L"Uzycie: cd <katalog>\r\n\r\n");
        return;
    }

    if (g_cwd == NULL) {
        Print(L"[ERR] cd: g_cwd == NULL\r\n\r\n");
        return;
    }

    // ASCII -> UTF-16
    CHAR16 dirname[256];
    UINTN i = 0;
    while (arg[i] && i < 255) {
        dirname[i] = (CHAR16)arg[i];
        i++;
    }
    dirname[i] = 0;

    EFI_FILE_PROTOCOL* new_dir = NULL;
    EFI_STATUS ST = uefi_call_wrapper(
        g_cwd->Open,
        5,
        g_cwd,
        &new_dir,
        dirname,
        EFI_FILE_MODE_READ,
        0   // atrybuty tylko przy tworzeniu, tu 0
    );

    if (EFI_ERROR(ST) || new_dir == NULL) {
        Print(L"[ERR] Nie moge otworzyc katalogu '%s': %r\r\n\r\n", dirname, ST);
        return;
    }

    // sprawdź, czy to katalog
    UINTN info_size = 0;
    EFI_FILE_INFO* info = NULL;
    EFI_GUID file_info_guid = EFI_FILE_INFO_ID;

    ST = uefi_call_wrapper(new_dir->GetInfo, 4, new_dir, &file_info_guid, &info_size, NULL);
    if (ST != EFI_BUFFER_TOO_SMALL) {
        Print(L"[ERR] GetInfo(size) failed: %r\r\n\r\n", ST);
        uefi_call_wrapper(new_dir->Close, 1, new_dir);
        return;
    }

    ST = uefi_call_wrapper(
        g_st->BootServices->AllocatePool,
        3, EfiLoaderData, info_size, (void**)&info
    );
    if (EFI_ERROR(ST)) {
        Print(L"[ERR] AllocatePool failed: %r\r\n\r\n", ST);
        uefi_call_wrapper(new_dir->Close, 1, new_dir);
        return;
    }

    ST = uefi_call_wrapper(new_dir->GetInfo, 4, new_dir, &file_info_guid, &info_size, info);
    if (EFI_ERROR(ST)) {
        Print(L"[ERR] GetInfo failed: %r\r\n\r\n", ST);
        uefi_call_wrapper(g_st->BootServices->FreePool, 1, info);
        uefi_call_wrapper(new_dir->Close, 1, new_dir);
        return;
    }

    if (!(info->Attribute & EFI_FILE_DIRECTORY)) {
        Print(L"[ERR] '%s' nie jest katalogiem.\r\n\r\n", dirname);
        uefi_call_wrapper(g_st->BootServices->FreePool, 1, info);
        uefi_call_wrapper(new_dir->Close, 1, new_dir);
        return;
    }

    if (g_cwd != NULL && g_cwd != g_root)
        uefi_call_wrapper(g_cwd->Close, 1, g_cwd);

    g_cwd = new_dir;

    uefi_call_wrapper(g_st->BootServices->FreePool, 1, info);

    Print(L"Zmieniono katalog na '%s'\r\n\r\n", dirname);
}

// ===================== PRZYGOTOWANIE ELFA DO EXIT BOOT SERVICES =========================

static EFI_STATUS prepare_elf(void) {
    if (g_bin_buf == NULL || g_bin_size == 0)
        return EFI_NOT_READY;

    UINTN entry = 0, base = 0;
    UINTN res = load_elf_advanced(g_bin_buf, g_bin_size, &entry, &base);
    if (res == 0)
        return EFI_LOAD_ERROR;

    g_elf_entry = entry;
    g_elf_base  = base;
    return EFI_SUCCESS;
}

// ===================== URUCHAMIANIE ELFA PO EXIT BOOT SERVICES =======================

static void run_elf_bare(void) {
    raw_entry_t entry_fn = (raw_entry_t)g_elf_entry;
    entry_fn(&g_exports);

    for (;;) __asm__("hlt");
}

// ===================== "KERNEL" W TYM SAMYM PLIKU =====================

void kernel_main(void)
{
    EFI_STATUS Status;

    extern EFI_HANDLE g_image_handle;

    cls();
    Print(L"Terminal\r\n\r\n");
    Print(L"Nie ma obslugi polskich znakow\r\n");
    Print(L"Wpisz 'help' po spis komend i ich opis\r\n\r\n");

    char line[128];

    for (;;) {
        Print(L"> ");
        read_line(line, sizeof(line));

        // proste parsowanie: komenda + argument
        parsed_cmd p = lex(line);
        if (p.count == 0)
            continue;

        char *cmd = p.tokens[0];
        char *arg = (p.count >= 2 ? p.tokens[1] : NULL);


        if (my_strcmp(cmd, "shutdown") == 0) {
            // w kernelu to raczej zawiecha niż wyjście
            uefi_call_wrapper(
                ST->RuntimeServices->ResetSystem,
                4,
                EfiResetShutdown,
                EFI_SUCCESS,
                0,
                NULL
        );

        } else if (my_strcmp(cmd, "exit") == 0) {
            uefi_call_wrapper(ST->BootServices->Exit,
                  2,
                  IH,
                  EFI_SUCCESS);

        } else if (my_strcmp(cmd, "help") == 0) {
            Print(L"exit - wychodzi z systemu\r\n");
            Print(L"reboot - resetuje system\r\n");
            Print(L"shutdown - wylacza maszyne\r\n");
            Print(L"cls - czysci ekran\r\n");
            Print(L"cd <lokalizacja> - wejdz w inna lokalizacje\r\n");
            Print(L"load <plik.bin> - ladowanie surowej binarki albo ELF\r\n");
            Print(L"load_elf - ladowanie zaladowanego ELF do pamieci wykonywalnej\r\n");
            Print(L"run_elf - uruchamia ELFa\r\n");
            Print(L"run <adres w hexie> - skok pod adres zaladowanej surowej binarki\r\n");
            Print(L"UWAGA! jak skaczesz pod adres, to wpisuj adres bez '0x' na poczatku\r\n");
            Print(L"autorun - wykonuje exit boot services i skacze do entry pointa ELFa");
            Print(L"triple_fault - triple fault\r\n");
            Print(L"exit_boot_services - konczy wsparcie UEFI dla kodu\r\n");
            Print(L"ls - pokazuje liste plikow i folderow\r\n\r\n");

            Print(L"Ladujesz ELFa przez 'load', potem load_elf i run_elf\r\n");
            Print(L"Dla surowej binarki bedzie to 'load', potem run <adres>, ale bez '0x' na poczatku\r\n\r\n");
        } else if (my_strcmp(cmd, "cls") == 0) {
            cls();
        } else if (my_strcmp(cmd, "cd") == 0) {
            cmd_cd(arg);
        } else if (my_strcmp(cmd, "load") == 0) {
            cmd_load(arg);
        } else if (my_strcmp(cmd, "reboot") == 0) {
            uefi_call_wrapper(
                ST->RuntimeServices->ResetSystem,
                4,                 // liczba argumentów
                EfiResetCold,      // typ resetu
                EFI_SUCCESS,       // status
                0,                 // rozmiar danych
                NULL               // dane
            );

        } else if (my_strcmp(cmd, "triple_fault") == 0) {
            trigger_triple_fault();

        } else if (my_strcmp(cmd, "exit_boot_services") == 0) {
                // --- WYJŚCIE Z BOOT SERVICES ---
                // użyj globalnego IH albo g_image_handle – ważne, żeby to był ten sam handle,
                // którego dostałeś w efi_main
                Status = do_exit_boot_services(IH);
                if (EFI_ERROR(Status)) {
                    Print(L"[BOOT] ExitBootServices nie powiodl sie: %r\r\n", Status);
                    return;    // nie możemy zwrócić EFI_STATUS z funkcji typu void
                }
        } else if (my_strcmp(cmd, "ls") == 0) {
            extern EFI_HANDLE g_image_handle;
            cmd_ls();
        
        } else if (my_strcmp(cmd, "run") == 0) {
            cmd_run(arg);
        
        } else if (my_strcmp(cmd, "load_elf") == 0) {
            UINTN entry = 0;
            UINTN base = 0;

            load_elf_advanced(g_bin_buf, g_bin_size, &entry, &base);
        } else if (my_strcmp(cmd, "autorun") == 0) {

        if (EFI_ERROR(prepare_elf())) {
            Print(L"[ELF] Nie moge przygotowac ELF\r\n");
            return;
        }

        Status = do_exit_boot_services(IH);
        if (EFI_ERROR(Status)) {
            Print(L"[BOOT] ExitBootServices nie powiodl sie: %r\r\n", Status);
            return;
        }

        // Po ExitBootServices ZERO Print, ZERO BootServices
        run_elf_bare();

        } else if (my_strcmp(cmd, "run_elf") == 0) {
            cmd_runelf();
        } else {
            Print(L"Niepoprawna komenda\r\n\r\n");
        }
    }
}


// ===================== EFI MAIN =====================

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{

// ==================== POINTERY POINTERÓW (CHYBA) ==========================

    IH = ImageHandle;
    ST = SystemTable;

// ===================== WŁAŚCIWY BOOTLOADER =========================

    InitializeLib(ImageHandle, SystemTable);
    g_st = SystemTable;
    g_image_handle = ImageHandle;

    // wyłączenie watchdog
    uefi_call_wrapper(SystemTable->BootServices->SetWatchdogTimer,
                      4, 0, 0, 0, NULL);

    // --- FS: LoadedImage -> SimpleFileSystem -> Root ---
    EFI_STATUS status;
    EFI_LOADED_IMAGE* loaded_image = NULL;
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    status = uefi_call_wrapper(
        ST->BootServices->HandleProtocol,
        3, ImageHandle, &loaded_image_guid, (void**)&loaded_image
    );
    if (EFI_ERROR(status) || loaded_image == NULL) {
        Print(L"[ERR] HandleProtocol(LoadedImage) = %r\r\n", status);
        return status;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    status = uefi_call_wrapper(
        ST->BootServices->HandleProtocol,
        3, loaded_image->DeviceHandle, &fs_guid, (void**)&fs
    );
    if (EFI_ERROR(status) || fs == NULL) {
        Print(L"[ERR] HandleProtocol(SimpleFS) = %r\r\n", status);
        return status;
    }

    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &g_root);
    if (EFI_ERROR(status) || g_root == NULL) {
        Print(L"[ERR] OpenVolume = %r\r\n", status);
        return status;
    }

    g_cwd = g_root;

    // 1. Pobierz GOP
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol,
                               3, &gopGuid, NULL, (void**)&g_gop);

    if (EFI_ERROR(status) || g_gop == NULL) {
        Print(L"[BOOT] LocateProtocol(GOP) failed: %r\n", status);
        return status;
    }

    g_fb_width  = g_gop->Mode->Info->HorizontalResolution;
    g_fb_height = g_gop->Mode->Info->VerticalResolution;
    g_fb_pitch  = g_gop->Mode->Info->PixelsPerScanLine;
    g_fb        = (uint32_t*)g_gop->Mode->FrameBufferBase;

    // EXPORTY SYMBOLI DO FRAMEBUFFERA

    g_exports.fb        = g_fb;
    g_exports.fb_width  = g_fb_width;
    g_exports.fb_height = g_fb_height;
    g_exports.fb_pitch  = g_fb_pitch;

    Print(L"[BOOT] GOP: %ux%u pitch=%u fb=0x%lx\n",
          g_fb_width, g_fb_height, g_fb_pitch,
          (UINT64)g_fb);

    Print(L"[BOOT] skaczemy do kernela...\n");

    // 2. Skok do "kernela" (w tym samym pliku)
    kernel_main();

    Print(L"[BOOT] kernel_main zwrocil (nie powinien)\n");

    return EFI_SUCCESS;
}