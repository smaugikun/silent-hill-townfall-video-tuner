/* Writes Scripts/tf_video.profile (profile.h) for the game it is given, from the game's own PDB
 * (Townfall-Win64-Shipping.pdb, next to its program), read with Windows' dbghelp.dll. tf_video.dll runs this when its
 * profile doesn't fit the running game, after a game update: through Windows' rundll32.exe, in a process of its own at
 * low priority, because dbghelp takes about 1.7 GB for this PDB for a few seconds, which the game must not carry.
 * Everything the DLL takes for granted is checked first: each function's arguments and result, each member it reads
 * with its size, and a few layouts. A game update that changed any of that gets no profile, and the DLL stays off
 * rather than guess.
 *
 *   rundll32.exe tf_video.dll,tf_video_profile <Townfall-Win64-Shipping.exe> <profile> [--log <file>]
 *                [--expect <function> <signature>]
 *
 * --log appends the outcome to the mod's log. --expect replaces one function's expected signature (the tests' way of
 * making a game that changed). Exit code 0: written; 2: the game changed what the DLL relies on; 3: a file couldn't be
 * read or written. */
#include <windows.h>
#include <dbghelp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "profile.h"

enum { WRITTEN = 0, USAGE = 1, CHANGED = 2, UNREADABLE = 3 };
/* The SymTagEnum values used here (DIA's cvconst.h, which MinGW's dbghelp.h doesn't have). */
enum { TAG_FUNCTION = 5, TAG_POINTER = 14, TAG_BASE = 16 };
static int failure;      /* CHANGED or UNREADABLE: the first problem found, said by `reason` */
static char reason[1024];

static int fail(int kind, const char *format, ...) {
    if (!failure) {
        va_list args;
        va_start(args, format);
        vsnprintf(reason, sizeof(reason), format, args);
        va_end(args);
        failure = kind;
    }
    return 0;
}

/* The game's program, mapped: its build stamp, loaded size, PDB identity, and bytes by RVA. */
static const unsigned char *program;
static uint64_t program_size;
static const IMAGE_SECTION_HEADER *sections;
static unsigned section_count;

static const unsigned char *program_data(uint32_t rva, uint32_t size) {
    for (unsigned i = 0; i < section_count; ++i) {
        const IMAGE_SECTION_HEADER *s = &sections[i];
        uint64_t loaded = s->Misc.VirtualSize < s->SizeOfRawData ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (rva >= s->VirtualAddress && (uint64_t)rva + size <= s->VirtualAddress + loaded &&
            s->PointerToRawData + (uint64_t)(rva - s->VirtualAddress) + size <= program_size)
            return program + s->PointerToRawData + (rva - s->VirtualAddress);
    }
    return NULL;
}

static int open_program(const wchar_t *path, TfProfile *profile) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return fail(UNREADABLE, "can't open the game's program (Windows error %lu)", GetLastError());
    LARGE_INTEGER size;
    HANDLE mapping = GetFileSizeEx(file, &size) && size.QuadPart >= 0x400 ?
        CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    CloseHandle(file);
    if (!mapping) return fail(UNREADABLE, "can't read the game's program");
    program = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (!program) return fail(UNREADABLE, "can't read the game's program");
    program_size = (uint64_t)size.QuadPart;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)program;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (uint64_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > program_size) return fail(UNREADABLE, "the game's program isn't a Windows program");
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(program + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail(UNREADABLE, "the game's program isn't a 64-bit Windows program");
    sections = IMAGE_FIRST_SECTION(nt);
    section_count = nt->FileHeader.NumberOfSections;
    if ((const unsigned char *)(sections + section_count) > program + program_size)
        return fail(UNREADABLE, "the game's program is cut off");
    profile->timestamp = nt->FileHeader.TimeDateStamp;
    profile->image_size = nt->OptionalHeader.SizeOfImage;
    /* The CodeView record that ties the program to its PDB ("RSDS", the PDB's GUID and age): tf_video.dll checks it. */
    IMAGE_DATA_DIRECTORY debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    for (uint32_t at = 0; at + sizeof(IMAGE_DEBUG_DIRECTORY) <= debug.Size; at += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        const IMAGE_DEBUG_DIRECTORY *entry = (const void *)program_data(debug.VirtualAddress + at, sizeof(IMAGE_DEBUG_DIRECTORY));
        const unsigned char *record = entry && entry->Type == IMAGE_DEBUG_TYPE_CODEVIEW && entry->SizeOfData >= 24 ?
            program_data(entry->AddressOfRawData, 24) : NULL;
        if (record && !memcmp(record, "RSDS", 4)) { memcpy(profile->identity, record, 24); return 1; }
    }
    return fail(UNREADABLE, "the game's program names no PDB");
}

/* Windows' own dbghelp.dll, from System32 only: never one that lies next to this program. */
static struct {
    DWORD (WINAPI *SymSetOptions)(DWORD);
    BOOL (WINAPI *SymInitializeW)(HANDLE, PCWSTR, BOOL);
    DWORD64 (WINAPI *SymLoadModuleExW)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD, PMODLOAD_DATA, DWORD);
    BOOL (WINAPI *SymGetModuleInfoW64)(HANDLE, DWORD64, PIMAGEHLP_MODULEW64);
    BOOL (WINAPI *SymFromName)(HANDLE, PCSTR, PSYMBOL_INFO);
    BOOL (WINAPI *SymGetTypeFromName)(HANDLE, ULONG64, PCSTR, PSYMBOL_INFO);
    BOOL (WINAPI *SymGetTypeInfo)(HANDLE, DWORD64, ULONG, IMAGEHLP_SYMBOL_TYPE_INFO, PVOID);
} dbg;
static HANDLE process;
static DWORD64 base;

static int open_symbols(const wchar_t *program_path) {
    HMODULE dll = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    struct { const char *name; void *slot; } entries[] = {
        {"SymSetOptions", &dbg.SymSetOptions}, {"SymInitializeW", &dbg.SymInitializeW},
        {"SymLoadModuleExW", &dbg.SymLoadModuleExW}, {"SymGetModuleInfoW64", &dbg.SymGetModuleInfoW64},
        {"SymFromName", &dbg.SymFromName}, {"SymGetTypeFromName", &dbg.SymGetTypeFromName},
        {"SymGetTypeInfo", &dbg.SymGetTypeInfo}};
    for (size_t i = 0; i < sizeof(entries)/sizeof(*entries); ++i) {
        FARPROC found = dll ? GetProcAddress(dll, entries[i].name) : NULL;
        if (!found) return fail(UNREADABLE, "Windows' dbghelp.dll lacks %s", entries[i].name);
        memcpy(entries[i].slot, &found, sizeof(found));
    }
    wchar_t folder[MAX_PATH * 2];
    wcsncpy(folder, program_path, MAX_PATH * 2 - 1); folder[MAX_PATH * 2 - 1] = 0;
    wchar_t *end = wcsrchr(folder, L'\\');
    if (end) *end = 0;
    process = GetCurrentProcess();
    /* Undecorated names, exact symbols only, no dialogs, no symbol server: the PDB next to the program. */
    dbg.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_EXACT_SYMBOLS | SYMOPT_IGNORE_NT_SYMPATH |
                      SYMOPT_NO_PROMPTS);
    if (!dbg.SymInitializeW(process, folder, FALSE)) return fail(UNREADABLE, "dbghelp didn't start (Windows error %lu)", GetLastError());
    base = dbg.SymLoadModuleExW(process, NULL, program_path, NULL, 0, 0, NULL, 0);
    IMAGEHLP_MODULEW64 module = {.SizeOfStruct = sizeof(module)};
    if (!base || !dbg.SymGetModuleInfoW64(process, base, &module) || module.SymType != SymPdb)
        return fail(UNREADABLE, "no Townfall-Win64-Shipping.pdb for this game next to its program");
    return 1;
}

static int query(ULONG index, IMAGEHLP_SYMBOL_TYPE_INFO kind, void *out) {
    return dbg.SymGetTypeInfo(process, base, index, kind, out);
}
static int query32(ULONG index, IMAGEHLP_SYMBOL_TYPE_INFO kind, DWORD *out) {
    *out = 0;
    return query(index, kind, out);
}
static int name_of(ULONG index, char *out, size_t size) {
    WCHAR *name = NULL;
    if (!query(index, TI_GET_SYMNAME, &name) || !name) return 0;
    int ok = WideCharToMultiByte(CP_UTF8, 0, name, -1, out, (int)size, NULL, NULL) > 0;
    LocalFree(name);
    return ok;
}

/* A type as text: its name; a pointer or reference with *; a base type as base<kind>:<bytes>; another unnamed type as
 * <tag><its target> ("None" where dbghelp says nothing). The expected signatures in profile.h are written this way. */
static void type_text(ULONG index, int depth, char *out, size_t size) {
    if (name_of(index, out, size)) return;
    DWORD tag, target, kind;
    ULONG64 length;
    int has_tag = query32(index, TI_GET_SYMTAG, &tag), has_target = query32(index, TI_GET_TYPEID, &target);
    char tag_text[16] = "None";
    if (has_tag) snprintf(tag_text, sizeof(tag_text), "%lu", tag);
    if (depth < 5 && has_target && target != index) {
        char inner[1024];
        type_text(target, depth + 1, inner, sizeof(inner));
        if (has_tag && tag == TAG_POINTER) snprintf(out, size, "%s*", inner);
        else if (has_tag && tag == TAG_BASE) snprintf(out, size, "baseNone:None");
        else snprintf(out, size, "%s<%s>", tag_text, inner);
        return;
    }
    if (has_tag && tag == TAG_POINTER) { snprintf(out, size, "?*"); return; }
    char kind_text[16] = "None", length_text[24] = "None";
    if (query32(index, TI_GET_BASETYPE, &kind)) snprintf(kind_text, sizeof(kind_text), "%lu", kind);
    if (query(index, TI_GET_LENGTH, &length)) snprintf(length_text, sizeof(length_text), "%llu", (unsigned long long)length);
    snprintf(out, size, "base%s:%s", kind_text, length_text);
}

/* A type's children (members, bases, arguments, enum values), or NULL with `count` 0 for none. */
static ULONG *children(ULONG index, DWORD *count) {
    if (!query32(index, TI_GET_CHILDRENCOUNT, count) || !*count) { *count = 0; return NULL; }
    if (*count > 4096) { fail(CHANGED, "an unexpectedly large type"); *count = 0; return NULL; }
    TI_FINDCHILDREN_PARAMS *params = calloc(1, sizeof(*params) + *count * sizeof(ULONG));
    if (!params) { *count = 0; return NULL; }
    params->Count = *count;
    if (!query(index, TI_FINDCHILDREN, params)) {
        fail(UNREADABLE, "dbghelp couldn't list a type's members (Windows error %lu)", GetLastError());
        free(params); *count = 0; return NULL;
    }
    ULONG *ids = malloc(*count * sizeof(ULONG));
    if (ids) memcpy(ids, params->ChildId, *count * sizeof(ULONG));
    free(params);
    if (!ids) *count = 0;
    return ids;
}

static void signature_text(ULONG function_type, char *out, size_t size) {
    DWORD result, argument, count;
    char text[1024];
    if (query32(function_type, TI_GET_TYPEID, &result)) type_text(result, 0, text, sizeof(text));
    else snprintf(text, sizeof(text), "?");
    size_t used = (size_t)snprintf(out, size, "%s(", text);
    ULONG *ids = children(function_type, &count);
    for (DWORD i = 0; i < count && used < size; ++i) {
        if (query32(ids[i], TI_GET_TYPEID, &argument)) type_text(argument, 0, text, sizeof(text));
        else snprintf(text, sizeof(text), "?");
        used += (size_t)snprintf(out + used, size - used, "%s%s", i ? ", " : "", text);
    }
    free(ids);
    if (used < size) snprintf(out + used, size - used, ")");
}

typedef union { SYMBOL_INFO info; char room[sizeof(SYMBOL_INFO) + 2048]; } Symbol;

static int function(const char *name, const char *expected, TfProfile *profile, int key) {
    Symbol symbol = {.info = {.SizeOfStruct = sizeof(SYMBOL_INFO), .MaxNameLen = 2048}};
    if (!dbg.SymFromName(process, name, &symbol.info)) return fail(CHANGED, "%s is gone", name);
    if (symbol.info.Tag != TAG_FUNCTION || !symbol.info.TypeIndex) return fail(CHANGED, "%s isn't a function", name);
    char text[4096];
    signature_text(symbol.info.TypeIndex, text, sizeof(text));
    if (strcmp(text, expected)) return fail(CHANGED, "%s is now %s, not %s", name, text, expected);
    if (!symbol.info.Size) return fail(CHANGED, "%s has no size", name);
    uint32_t rva = (uint32_t)(symbol.info.Address - base);
    const unsigned char *code = program_data(rva, 16);
    if (!code) return fail(UNREADABLE, "%s isn't in the game's program", name);
    profile->functions[key].rva = rva;
    memcpy(profile->functions[key].code, code, 16);
    return 1;
}

static int type_index(const char *name, ULONG *index) {
    Symbol symbol = {.info = {.SizeOfStruct = sizeof(SYMBOL_INFO), .MaxNameLen = 2048}};
    if (!dbg.SymGetTypeFromName(process, base, name, &symbol.info)) return 0;
    *index = symbol.info.TypeIndex;
    return 1;
}
static int type_length(const char *name, uint64_t *out) {
    ULONG index;
    ULONG64 length;
    if (!type_index(name, &index) || !query(index, TI_GET_LENGTH, &length)) return 0;
    *out = length;
    return 1;
}
static int length_is(const char *name, uint64_t size) {
    uint64_t length;
    return type_length(name, &length) && length == size ? 1 : fail(CHANGED, "%s isn't %llu bytes any more", name,
                                                                    (unsigned long long)size);
}

/* The one data member (or base) `name` of `layout`: its offset, bit position if it has one, and type. */
typedef struct { DWORD offset, bit, type; int has_bit, has_type; } Member;

static int child(const char *layout, const char *name, Member *out) {
    ULONG index;
    DWORD count;
    if (!type_index(layout, &index)) return fail(CHANGED, "%s is gone", layout);
    ULONG *ids = children(index, &count);
    int found = 0;
    for (DWORD i = 0; i < count; ++i) {
        char text[1024];
        DWORD offset;
        if (!name_of(ids[i], text, sizeof(text)) || strcmp(text, name) || !query32(ids[i], TI_GET_OFFSET, &offset)) continue;
        ++found;
        out->offset = offset;
        out->has_bit = query32(ids[i], TI_GET_BITPOSITION, &out->bit);
        out->has_type = query32(ids[i], TI_GET_TYPEID, &out->type);
    }
    free(ids);
    return found == 1 ? 1 : fail(CHANGED, "%s has no %s", layout, name);
}

/* How many bytes a member takes: a named type's size, 8 for a pointer, a base type's own size. */
static int member_size(const Member *m, uint64_t *size) {
    char name[1024];
    DWORD tag, target;
    ULONG64 length;
    if (!m->has_type) return 0;
    if (name_of(m->type, name, sizeof(name))) return type_length(name, size);
    if (query32(m->type, TI_GET_SYMTAG, &tag) && tag == TAG_POINTER) { *size = 8; return 1; }
    if (query32(m->type, TI_GET_TYPEID, &target) && target != m->type) return 0;
    if (!query(m->type, TI_GET_LENGTH, &length)) return 0;
    *size = length;
    return 1;
}

/* A member's offset in `layout`; `size` (unless negative): the bytes the DLL reads there, all its own. */
static int member(const char *layout, const char *name, int size, uint32_t *offset) {
    Member m;
    uint64_t actual;
    if (!child(layout, name, &m)) return 0;
    if (size >= 0 && (!member_size(&m, &actual) || actual != (uint64_t)size || m.has_bit))
        return fail(CHANGED, "%s::%s isn't %d bytes of its own", layout, name, size);
    *offset = m.offset;
    return 1;
}
static int member_at(const char *layout, const char *name, int size, uint32_t offset) {
    uint32_t actual;
    return member(layout, name, size, &actual) && (actual == offset || fail(CHANGED, "%s::%s moved", layout, name));
}

static int enum_value(const char *layout, const char *name, uint32_t *value) {
    ULONG index;
    DWORD count;
    if (!type_index(layout, &index)) return fail(CHANGED, "%s is gone", layout);
    ULONG *ids = children(index, &count);
    int found = 0;
    for (DWORD i = 0; i < count; ++i) {
        char text[1024];
        unsigned char variant[24];  /* a VARIANT: its type, then the integer at 8 */
        if (!name_of(ids[i], text, sizeof(text)) || strcmp(text, name) || !query(ids[i], TI_GET_VALUE, variant)) continue;
        uint16_t kind;
        memcpy(&kind, variant, 2);
        int64_t number = 0;
        switch (kind) {
        case VT_I1: { int8_t v; memcpy(&v, variant + 8, 1); number = v; break; }
        case VT_UI1: { uint8_t v; memcpy(&v, variant + 8, 1); number = v; break; }
        case VT_I2: { int16_t v; memcpy(&v, variant + 8, 2); number = v; break; }
        case VT_UI2: { uint16_t v; memcpy(&v, variant + 8, 2); number = v; break; }
        case VT_I4: { int32_t v; memcpy(&v, variant + 8, 4); number = v; break; }
        case VT_UI4: { uint32_t v; memcpy(&v, variant + 8, 4); number = v; break; }
        case VT_I8: case VT_UI8: memcpy(&number, variant + 8, 8); break;
        default: continue;
        }
        ++found;
        *value = (uint32_t)number;
    }
    free(ids);
    return found == 1 ? 1 : fail(CHANGED, "%s has no %s", layout, name);
}

/* What the DLL relies on beyond the functions: sizes, offsets and enum values (tf_video.c reads them through PV). */
static int read_values(uint32_t *v) {
    static const struct { const char *name; uint64_t size; } lengths[] = {
        {"FString", 16}, {"FTimespan", 8}, {"FName", 8}, {"FLinearColor", 16}, {"UE::Math::TVector2<double>", 16},
        {"FDrawToRenderTargetContext", 8}, {"TSharedPtr<ISoundGenerator,1>", 16}, {"FSoundGeneratorInitParams", 72},
        {"FWeakObjectPtr", 8}, {"FObjectPtr", 8}, {"FSignalDisplayData", 20}, {"FSignalNeedleWidgetData", 28}};
    for (size_t i = 0; i < sizeof(lengths)/sizeof(*lengths); ++i)
        if (!length_is(lengths[i].name, lengths[i].size)) return 0;
    uint32_t layout, offsets, left, desc, extent, format, flag;
    const char *radio = "UHandheldRadioComponent", *dial = "UCRTVTuningDisplayWidget", *needle = "FSignalNeedleWidgetData";
    if (!member("UMediaSoundComponent", "Channels", 4, &v[TFV_SOUND_CHANNELS]) ||
        !member("UMediaPlayer", "PlayOnOpen", 1, &v[TFV_MEDIA_PLAY_ON_OPEN]) ||
        !member("UMediaPlayer", "NativeAudioOut", 1, &v[TFV_MEDIA_NATIVE_AUDIO]) ||
        !member("UMediaPlayer", "PlayerFacade", 16, &v[TFV_MEDIA_FACADE]) ||
        !member("UMediaTexture", "NewStyleOutput", 1, &v[TFV_TEXTURE_NEW_STYLE]) ||
        !member("UMediaTexture", "Dimensions", 8, &v[TFV_TEXTURE_DIMENSIONS]) ||
        !member("UMediaTexture", "CurrentAspectRatio", 4, &v[TFV_TEXTURE_ASPECT]) ||
        !member(radio, "bFineTuning", 1, &v[TFV_RADIO_FINE]) || !member(radio, "bInActiveMode", 1, &v[TFV_RADIO_ACTIVE]) ||
        !member(radio, "cachedHighestSignalStrength", 4, &v[TFV_RADIO_STRENGTH]) ||
        !member(radio, "bTunedToSignalObjectFrequency", 1, &v[TFV_RADIO_STORY]) ||
        !member(radio, "NeedlesWidget", 8, &v[TFV_RADIO_DIAL]) ||
        !member(dial, "MaxNumberOfNeedleWidgets", 4, &v[TFV_DIAL_CAPACITY]) ||
        !member(dial, "TuningNeedleWidgets", 16, &v[TFV_DIAL_NEEDLES]) || !member(dial, "CanvasPanel", 8, &v[TFV_DIAL_PANEL]) ||
        !member(needle, "Image", 8, &v[TFV_NEEDLE_IMAGE]) || !member(needle, "CanvasPanelSlot", 8, &v[TFV_NEEDLE_SLOT]) ||
        !member(needle, "SignalComponent", 8, &v[TFV_NEEDLE_SOURCE]) ||
        !member("UCanvasPanelSlot", "LayoutData", -1, &layout) || !member("FAnchorData", "Offsets", -1, &offsets) ||
        !member("FMargin", "Left", 4, &left) || !member("FTexture", "TextureRHI", 8, &v[TFV_TEXTURE_RHI]) ||
        !member("FRHITexture", "TextureDesc", -1, &desc) || !member("FRHITextureDesc", "Extent", 8, &extent) ||
        !member("FRHITextureDesc", "Format", 1, &format)) return 0;
    v[TFV_SLOT_X] = layout + offsets + left;
    v[TFV_RHI_EXTENT] = desc + extent;
    v[TFV_RHI_FORMAT] = desc + format;
    if (!member_at("FTextureResource", "FTexture", -1, 0)) return 0;
    /* UActorComponent::bIsActive is a bit: the DLL sets it on the sound component it keeps unregistered. */
    Member active;
    if (!child("UActorComponent", "bIsActive", &active)) return 0;
    if (!active.has_bit || active.bit > 7) return fail(CHANGED, "UActorComponent::bIsActive isn't a bit any more");
    v[TFV_SOUND_ACTIVE] = active.offset;
    v[TFV_SOUND_ACTIVE_MASK] = 1u << active.bit;
    /* main.lua constructs its media objects with these flags (64 | 128). */
    if (!enum_value("EObjectFlags", "RF_MarkAsRootSet", &flag) || flag != 128 ||
        !enum_value("EObjectFlags", "RF_Transient", &flag) || flag != 64)
        return failure ? 0 : fail(CHANGED, "the object flags the Lua side uses changed");
    if (!enum_value("EPixelFormat", "PF_B8G8R8A8", &v[TFV_PIXEL_BGRA]) ||
        !enum_value("EPixelFormat", "PF_R8G8B8A8", &v[TFV_PIXEL_RGBA]) ||
        !enum_value("EFMODSystemContext::Type", "Runtime", &v[TFV_FMOD_RUNTIME]) ||
        !enum_value("EMediaSoundChannels", "Stereo", &v[TFV_SOUND_STEREO]) ||
        !enum_value("EBlendMode", "BLEND_Opaque", &v[TFV_BLEND_OPAQUE]) ||
        !enum_value("ESignalActiveState", "ActiveDiscovered", &v[TFV_SIGNAL_DISCOVERED])) return 0;
    /* The layouts tf_video.c declares itself (SignalDisplay, GeneratorParams) and reads through. */
    return member_at("FSignalDisplayData", "SignalComponent", 8, 0) && member_at("FSignalDisplayData", "bIsEnemySignal", 1, 8) &&
        member_at("FSignalDisplayData", "bSuppressSquelch", 1, 9) && member_at("FSignalDisplayData", "SignalActiveState", 4, 12) &&
        member_at("FSignalDisplayData", "TuningFrequency", 4, 16) &&
        member_at("FSoundGeneratorInitParams", "SampleRate", 4, 16) && member_at("FSoundGeneratorInitParams", "NumChannels", 4, 24) &&
        member_at("FSoundGeneratorInitParams", "NumFramesPerCallback", 4, 28) &&
        member_at("FSoundGeneratorInitParams", "AudioMixerNumOutputFrames", 4, 20) &&
        member_at("TSharedPtr<ISoundGenerator,1>", "Object", 8, 0) && member_at("FMediaSoundGenerator", "ISoundGenerator", -1, 0) &&
        member_at("FTimespan", "Ticks", 8, 0);
}

/* Written whole or not at all: tf_video.dll reads it once this program has ended. */
static int write_profile(const wchar_t *path, const TfProfile *profile) {
    wchar_t staged[MAX_PATH * 2];
    DWORD written = 0;
    if (swprintf(staged, MAX_PATH * 2, L"%ls.tmp", path) < 0) return fail(UNREADABLE, "the profile's path is too long");
    HANDLE file = CreateFileW(staged, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return fail(UNREADABLE, "can't write the profile (Windows error %lu)", GetLastError());
    BOOL ok = WriteFile(file, profile, sizeof(*profile), &written, NULL) && written == sizeof(*profile);
    CloseHandle(file);
    if (ok && MoveFileExW(staged, path, MOVEFILE_REPLACE_EXISTING)) return 1;
    DeleteFileW(staged);
    return fail(UNREADABLE, "can't write the profile (Windows error %lu)", GetLastError());
}

/* The outcome, in the mod's log next to the DLL's own lines (rundll32 has no console). */
static void report(const wchar_t *log_path, const char *format, ...) {
    char line[2048];
    va_list args;
    if (!log_path) return;
    va_start(args, format);
    int n = snprintf(line, sizeof(line), "[TF-VIDEO] %llu Game profile: ", (unsigned long long)GetTickCount64());
    vsnprintf(line + n, sizeof(line) - n - 1, format, args);
    va_end(args);
    strcat(line, "\n");
    HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    CloseHandle(file);
}

/* rundll32's command line as arguments: separated by spaces, each in double quotes if it has spaces (no escapes: no
 * path has a quote in it). Splits `line` in place. */
static int split(wchar_t *line, wchar_t **argv, int most) {
    int argc = 0;
    while (line && *line && argc < most) {
        while (*line == L' ' || *line == L'\t') ++line;
        if (!*line) break;
        wchar_t end = *line == L'"' ? L'"' : L' ';
        if (end == L'"') ++line;
        argv[argc++] = line;
        while (*line && *line != end && !(end == L' ' && *line == L'\t')) ++line;
        if (*line) *line++ = 0;
    }
    return argc;
}

static int profile_main(int argc, wchar_t **argv) {
    const wchar_t *log_path = NULL;
    const char *expected[TF_FUNCTION_COUNT];
    static char replaced[4096];
    for (int i = 0; i < TF_FUNCTION_COUNT; ++i) expected[i] = tf_function_info[i].signature;
    if (argc < 3) return USAGE;
    for (int i = 3; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--log") && i + 1 < argc) log_path = argv[++i];
        else if (!wcscmp(argv[i], L"--expect") && i + 2 < argc) {
            char name[512];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, name, sizeof(name), NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 2], -1, replaced, sizeof(replaced), NULL, NULL);
            for (int key = 0; key < TF_FUNCTION_COUNT; ++key)
                if (!strcmp(tf_function_info[key].name, name)) expected[key] = replaced;
            i += 2;
        } else return USAGE;
    }
    ULONGLONG began = GetTickCount64();
    static TfProfile profile;
    memcpy(profile.magic, "TFVPROF1", 8);
    profile.version = TF_PROFILE_VERSION;
    profile.size = sizeof(profile);
    int ok = open_program(argv[1], &profile) && open_symbols(argv[1]);
    for (int key = 0; ok && key < TF_FUNCTION_COUNT; ++key)
        ok = function(tf_function_info[key].name, expected[key], &profile, key);
    ok = ok && read_values(profile.values) && write_profile(argv[2], &profile);
    if (ok) {
        report(log_path, "written for this game version (%d functions, %d values) in %.1f s", TF_FUNCTION_COUNT,
               TF_VALUE_COUNT, (GetTickCount64() - began) / 1000.0);
        return WRITTEN;
    }
    if (failure == CHANGED)
        report(log_path, "this game version changed what the mod relies on (%s); the mod stays off until it is updated", reason);
    else report(log_path, "couldn't read the game's symbols: %s", reason);
    return failure == CHANGED ? CHANGED : UNREADABLE;
}

/* rundll32.exe tf_video.dll,tf_video_profile ... (rundll32 takes this W entry for that name): the process ends with
 * profile_main's exit code. argv[0] stands in for a program name, so the indexes match a command line's. */
__declspec(dllexport) void CALLBACK tf_video_profileW(HWND window, HINSTANCE instance, LPWSTR command, int show) {
    (void)window; (void)instance; (void)show;
    wchar_t *argv[16] = {L"rundll32"};
    ExitProcess((UINT)profile_main(1 + split(command, argv + 1, 15), argv));
}
