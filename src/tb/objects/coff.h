#pragma once
#include "../tb_internal.h"
#include <sys/stat.h>

#ifdef _WIN32
#define fileno _fileno
#define fstat  _fstat
#define stat   _stat
#define strdup _strdup
#endif

/*#if TB_HOST_ARCH == TB_HOST_X86_64
#include <emmintrin.h>
#endif*/

// IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES
#define COFF_CHARACTERISTICS_TEXT 0x60500020u
// IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_READ
#define COFF_CHARACTERISTICS_DATA 0xC0000040u
// IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ
#define COFF_CHARACTERISTICS_RODATA 0x40000040u
// IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_READ |
// IMAGE_SCN_ALIGN_16BYTES
#define COFF_CHARACTERISTICS_BSS 0xC0500080u
// IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_ALIGN_8BYTES | IMAGE_SCN_MEM_READ |
// IMAGE_SCN_MEM_DISCARDABLE
#define COFF_CHARACTERISTICS_CV 0x42100040u

#define IMAGE_SCN_LNK_NRELOC_OVFL 0x01000000

#define IMAGE_SYM_CLASS_EXTERNAL 0x0002
#define IMAGE_SYM_CLASS_STATIC   0x0003
#define IMAGE_SYM_CLASS_LABEL    0x0006
#define IMAGE_SYM_CLASS_FILE     0x0067

#define IMAGE_FILE_LINE_NUMS_STRIPPED 0x0004

#define IMAGE_REL_AMD64_ADDR64   0x0001
#define IMAGE_REL_AMD64_ADDR32   0x0002
#define IMAGE_REL_AMD64_ADDR32NB 0x0003
#define IMAGE_REL_AMD64_REL32    0x0004
#define IMAGE_REL_AMD64_REL32_1  0x0005
#define IMAGE_REL_AMD64_REL32_2  0x0006
#define IMAGE_REL_AMD64_REL32_3  0x0007
#define IMAGE_REL_AMD64_REL32_4  0x0008
#define IMAGE_REL_AMD64_REL32_5  0x0009
#define IMAGE_REL_AMD64_SECTION  0x000A
#define IMAGE_REL_AMD64_SECREL   0x000B

#define IMAGE_SCN_MEM_DISCARDABLE 0x02000000
#define IMAGE_SCN_MEM_EXECUTE     0x20000000
#define IMAGE_SCN_MEM_READ        0x40000000
#define IMAGE_SCN_MEM_WRITE       0x80000000

#define IMAGE_SCN_CNT_CODE                   0x00000020  /* Section contains code. */
#define IMAGE_SCN_CNT_INITIALIZED_DATA       0x00000040  /* Section contains initialized data. */
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA     0x00000080  /* Section contains uninitialized data. */

#define IMAGE_DIRECTORY_ENTRY_EXPORT          0   // Export Directory
#define IMAGE_DIRECTORY_ENTRY_IMPORT          1   // Import Directory
#define IMAGE_DIRECTORY_ENTRY_RESOURCE        2   // Resource Directory
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION       3   // Exception Directory
#define IMAGE_DIRECTORY_ENTRY_SECURITY        4   // Security Directory
#define IMAGE_DIRECTORY_ENTRY_BASERELOC       5   // Base Relocation Table
#define IMAGE_DIRECTORY_ENTRY_DEBUG           6   // Debug Directory
#define IMAGE_DIRECTORY_ENTRY_ARCHITECTURE    7   // Architecture Specific Data
#define IMAGE_DIRECTORY_ENTRY_GLOBALPTR       8   // RVA of GP
#define IMAGE_DIRECTORY_ENTRY_TLS             9   // TLS Directory
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG    10   // Load Configuration Directory
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT   11   // Bound Import Directory in headers
#define IMAGE_DIRECTORY_ENTRY_IAT            12   // Import Address Table
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT   13   // Delay Load Import Descriptors
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14   // COM Runtime descriptor

#define IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3

#define MD5_HASHBYTES 16

typedef struct {
    char name[16];
    char date[12];

    // Microsoft tools don't actually do anything with this
    char user_id[6];
    char group_id[6];

    char mode[8];
    char size[10];

    uint8_t newline[2];
    uint8_t contents[];
} COFF_ArchiveMemberHeader;

typedef struct {
    uint16_t sig1;
    uint16_t sig2;
    uint16_t version;
    uint16_t machine;
    uint32_t timestamp;
    uint32_t size_of_data;
    uint16_t ordinal_hint;

    uint16_t type      : 2;
    uint16_t name_type : 3;
    uint16_t reserved  : 11;
} COFF_ImportHeader;

typedef struct {
    uint32_t import_lookup_table; // RVA
    uint32_t timestamp;
    uint32_t forwarder_chain;
    uint32_t name;
    uint32_t import_address_table; // RVA; Thunk table
} COFF_ImportDirectory;

typedef struct COFF_SectionHeader {
    char name[8];
    union {
        uint32_t physical_address;
        uint32_t virtual_size;
    } misc;
    uint32_t virtual_address;
    uint32_t raw_data_size;
    uint32_t raw_data_pos;
    uint32_t pointer_to_reloc;
    uint32_t pointer_to_lineno;
    uint16_t num_reloc;
    uint16_t num_lineno;
    uint32_t characteristics;
} COFF_SectionHeader;
static_assert(sizeof(COFF_SectionHeader) == 40, "COFF Section header size != 40 bytes");

typedef struct COFF_FileHeader {
    uint16_t machine;
    uint16_t num_sections;
    uint32_t timestamp;
    uint32_t symbol_table;
    uint32_t symbol_count;
    uint16_t optional_header_size;
    uint16_t characteristics;
} COFF_FileHeader;
static_assert(sizeof(COFF_FileHeader) == 20, "COFF File header size != 20 bytes");

// NOTE: Symbols, relocations, and line numbers are 2 byte packed
#pragma pack(push, 2)
typedef struct COFF_ImageReloc {
    union {
        uint32_t VirtualAddress;
        uint32_t RelocCount;
    };
    uint32_t SymbolTableIndex;
    uint16_t Type;
} COFF_ImageReloc;
static_assert(sizeof(COFF_ImageReloc) == 10, "COFF Image Relocation size != 10 bytes");

typedef struct COFF_Symbol {
    union {
        uint8_t  short_name[8];
        uint32_t long_name[2];
    };
    uint32_t value;
    int16_t  section_number;
    uint16_t type;
    uint8_t  storage_class;
    uint8_t  aux_symbols_count;
} COFF_Symbol;
static_assert(sizeof(COFF_Symbol) == 18, "COFF Symbol size != 18 bytes");

typedef struct COFF_AuxSectionSymbol {
    uint32_t length;       // section length
    uint16_t reloc_count;  // number of relocation entries
    uint16_t lineno_count; // number of line numbers
    uint32_t checksum;     // checksum for communal
    int16_t  number;       // section number to associate with
    uint8_t  selection;    // communal selection type
    uint8_t  reserved;
    int16_t  high_bits;    // high bits of the section number
} COFF_AuxSectionSymbol;
static_assert(sizeof(COFF_AuxSectionSymbol) == 18, "COFF Aux Section Symbol size != 18 bytes");

typedef union COFF_SymbolUnion {
    COFF_Symbol s;
    COFF_AuxSectionSymbol a;
} COFF_SymbolUnion;

typedef struct {
    union {
        unsigned long l_symndx; /* function name symbol index */
        unsigned long l_paddr;  /* address of line number     */
    } l_addr;
    unsigned short l_lnno; /* line number                */
} LINENO;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint16_t e_magic;    // Magic number
    uint16_t e_cblp;     // Bytes on last page of file
    uint16_t e_cp;       // Pages in file
    uint16_t e_crlc;     // Relocations
    uint16_t e_cparhdr;  // Size of header in paragraphs
    uint16_t e_minalloc; // Minimum extra paragraphs needed
    uint16_t e_maxalloc; // Maximum extra paragraphs needed
    uint16_t e_ss;       // Initial (relative) SS value
    uint16_t e_sp;       // Initial SP value
    uint16_t e_csum;     // Checksum
    uint16_t e_ip;       // Initial IP value
    uint16_t e_cs;       // Initial (relative) CS value
    uint16_t e_lfarlc;   // File address of relocation table
    uint16_t e_ovno;     // Overlay number
    uint16_t e_res[4];   // Reserved words
    uint16_t e_oemid;    // OEM identifier (for e_oeminfo)
    uint16_t e_oeminfo;  // OEM information; e_oemid specific
    uint16_t e_res2[10]; // Reserved words
    uint32_t e_lfanew;   // File address of new exe header
} PE_DosHeader;

typedef struct {
    uint32_t magic; // PE\0\0 or 0x00004550
    uint16_t machine;
    uint16_t section_count;
    uint32_t timestamp;
    uint32_t symbol_table;
    uint32_t symbol_count;
    uint16_t size_of_optional_header;
    uint16_t characteristics;
} PE_Header;

typedef struct {
    uint32_t virtual_address;
    uint32_t size;
} PE_ImageDataDirectory;

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
typedef struct {
    uint16_t magic;
    uint8_t major_linker_version;
    uint8_t minor_linker_version;
    uint32_t size_of_code;
    uint32_t size_of_initialized_data;
    uint32_t size_of_uninitialized_data;
    uint32_t entrypoint;
    uint32_t base_of_code;
    uint64_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t major_os_ver;
    uint16_t minor_os_ver;
    uint16_t major_image_ver;
    uint16_t minor_image_ver;
    uint16_t major_subsystem_ver;
    uint16_t minor_subsystem_ver;
    uint32_t win32_version_value;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t size_of_stack_reserve;
    uint64_t size_of_stack_commit;
    uint64_t size_of_heap_reserve;
    uint64_t size_of_heap_commit;
    uint32_t loader_flags;
    uint32_t rva_size_count;
    PE_ImageDataDirectory data_directories[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} PE_OptionalHeader64;

typedef struct { // size 40 bytes
    char name[8];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t pointer_to_relocs;
    uint32_t pointer_to_linenos;
    uint16_t relocation_count;
    uint16_t linenos_count;
    uint32_t characteristics;
} PE_SectionHeader;


typedef struct {
    unsigned long ptrtype     :5; // ordinal specifying pointer type (CV_ptrtype_e)
    unsigned long ptrmode     :3; // ordinal specifying pointer mode (CV_ptrmode_e)
    unsigned long isflat32    :1; // true if 0:32 pointer
    unsigned long isvolatile  :1; // TRUE if volatile pointer
    unsigned long isconst     :1; // TRUE if const pointer
    unsigned long isunaligned :1; // TRUE if unaligned pointer
    unsigned long isrestrict  :1; // TRUE if restricted pointer (allow agressive opts)
    unsigned long size        :6; // size of pointer (in bytes)
    unsigned long ismocom     :1; // TRUE if it is a MoCOM pointer (^ or %)
    unsigned long islref      :1; // TRUE if it is this pointer of member function with & ref-qualifier
    unsigned long isrref      :1; // TRUE if it is this pointer of member function with && ref-qualifier
    unsigned long unused      :10;// pad out to 32-bits for following cv_typ_t's
} CV_LFPointerAttribs;

typedef struct {
    uint16_t len;
    uint16_t leaf;  // LF_POINTER
    uint32_t utype; // type index of the underlying type
    CV_LFPointerAttribs attr;
    union {
        struct {
            uint32_t pmclass;    // index of containing class for pointer to member
            uint16_t pmenum;     // enumeration specifying pm format (CV_pmtype_e)
        } pm;
    } pbase;
} CV_LFPointer;

typedef struct {
    uint16_t packed        :1; // true if structure is packed
    uint16_t ctor          :1; // true if constructors or destructors present
    uint16_t ovlops        :1; // true if overloaded operators present
    uint16_t isnested      :1; // true if this is a nested class
    uint16_t cnested       :1; // true if this class contains nested types
    uint16_t opassign      :1; // true if overloaded assignment (=)
    uint16_t opcast        :1; // true if casting methods
    uint16_t fwdref        :1; // true if forward reference (incomplete defn)
    uint16_t scoped        :1; // scoped definition
    uint16_t hasuniquename :1; // true if there is a decorated name following the regular name
    uint16_t sealed        :1; // true if class cannot be used as a base class
    uint16_t hfa           :2; // CV_HFA_e
    uint16_t intrinsic     :1; // true if class is an intrinsic type (e.g. __m128d)
    uint16_t mocom         :2; // CV_MOCOM_UDT_e
} CV_Prop;

typedef struct {
    uint16_t access      :2; // access protection CV_access_t
    uint16_t mprop       :3; // method properties CV_methodprop_t
    uint16_t pseudo      :1; // compiler generated fcn and does not exist
    uint16_t noinherit   :1; // true if class cannot be inherited
    uint16_t noconstruct :1; // true if class cannot be constructed
    uint16_t compgenx    :1; // compiler generated fcn and does exist
    uint16_t sealed      :1; // true if method cannot be overridden
    uint16_t unused      :6; // unused
} CV_FieldAttrib;

typedef struct {
    uint16_t len;
    uint16_t leaf; // LF_FIELDLIST
} CV_LFFieldList;

typedef struct {
    uint16_t leaf; // LF_LONG
    int32_t val;   // signed 32-bit value
} CV_LFLong;

typedef struct {
    uint16_t leaf; // LF_QUAD
    int64_t val;   // signed 64-bit value
} CV_LFQuad;

typedef struct {
    uint16_t leaf; // LF_VARSTRING
    uint8_t val[];
} CV_LFVarString;

typedef struct {
    uint16_t       leaf;  // LF_MEMBER
    CV_FieldAttrib attr;  // attribute mask
    uint32_t       index; // index of type record for field
    // variable length offset of field followed
    // by length prefixed name of field
    uint8_t offset[];
} CV_LFMember;

typedef struct CV_LFStruct {
    uint16_t len;
    uint16_t leaf;    // LF_CLASS, LF_STRUCT, LF_INTERFACE
    uint16_t count;   // count of number of elements in class
    CV_Prop  property;// property attribute field (prop_t)
    uint32_t field;   // type index of LF_FIELD descriptor list
    uint32_t derived; // type index of derived from list if not zero
    uint32_t vshape;  // type index of vshape table for this class
    uint8_t  data[];  // data describing length of structure in bytes and name
} CV_LFStruct;

typedef enum {
    CV_LOCAL_IS_PARAM         = 1,   // variable is a parameter
    CV_LOCAL_IS_ADDR_TAKEN    = 2,   // address is taken
    CV_LOCAL_IS_COMPILER_GEND = 4,   // variable is compiler generated
    CV_LOCAL_IS_AGGREGATE     = 8,   // the symbol is splitted in temporaries, which are treated by compiler as independent entities
    CV_LOCAL_IS_AGGREGATED    = 16,  // Counterpart of fIsAggregate - tells that it is a part of a fIsAggregate symbol
    CV_LOCAL_IS_ALIASED       = 32,  // variable has multiple simultaneous lifetimes
    CV_LOCAL_IS_ALIAS         = 64,  // represents one of the multiple simultaneous lifetimes
    CV_LOCAL_IS_RETURN_VALUE  = 128, // represents a function return value
    CV_LOCAL_IS_OPTIMIZED_OUT = 256, // variable has no lifetimes
    CV_LOCAL_IS_ENREG_GLOBAL  = 512, // variable is an enregistered global
    CV_LOCAL_IS_ENREG_STATIC  = 1024,// variable is an enregistered static
} CV_LocalVarFlags;

// CV_Local is followed by CV_DefRange memory
typedef struct {
    uint16_t reclen; // Record length
    uint16_t rectyp; // S_LOCAL
    uint32_t typind; // type index
    uint16_t flags;  // local var flags (CV_LocalVarFlags)
    uint8_t  name[]; // Name of this symbol, a null terminated array of UTF8 characters.
} CV_Local;

typedef struct {
    uint32_t offset_start;
    uint16_t isect_start;
    uint16_t cb_range;
} CV_AddressRange;

// Represents the holes in overall address range, all address is pre-bbt.
// it is for compress and reduce the amount of relocations need.
typedef struct {
    uint16_t gap_start_offset; // relative offset from the beginning of the live range.
    uint16_t cb_range;         // length of this gap.
} CV_AddressGap;

// A live range of sub field of variable
typedef struct {
    uint16_t reclen;       // Record length
    uint16_t rectyp;       // S_DEFRANGE
    uint32_t program;      // DIA program to evaluate the value of the symbol
    CV_AddressRange range; // Range of addresses where this program is valid
    CV_AddressGap gaps[];  // The value is not available in following gaps
} CV_DefRange;

typedef struct {
    uint16_t reclen;       // Record length
    uint16_t rectyp;       // S_DEFRANGE_FRAMEPOINTER_REL
    int32_t local;
    CV_AddressRange range; // Range of addresses where this program is valid
    CV_AddressGap gaps[];  // The value is not available in following gaps
} CV_DefRangeFrameRel;

typedef struct {
    uint16_t reclen; // Record length
    uint16_t rectyp; // S_REGREL32
    uint32_t off;    // offset of symbol
    uint32_t typind; // Type index or metadata token
    uint16_t reg;    // register index for symbol
    uint8_t  name[]; // Length-prefixed name
} CV_RegRel32;
#pragma pack(pop)

// represents a CodeView type entry, they start with 16bits for length field
typedef struct CV_TypeEntry {
    uint32_t key;   // points to somewhere in the debug$T section, 0 is assumed to mean nothing
    uint16_t value; // type index
} CV_TypeEntry;

enum {
    COFF_MACHINE_AMD64 = 0x8664, // AMD64 (K8)
    COFF_MACHINE_ARM64 = 0xAA64, // ARM64 Little-Endian
};

enum {
    S_LDATA32        = 0x110c, // Module-local symbol
    S_GDATA32        = 0x110d, // Global data symbol
    S_LPROC32_ID     = 0x1146,
    S_GPROC32_ID     = 0x1147,
    S_INLINESITE     = 0x114d, // inlined function callsite.
    S_INLINESITE_END = 0x114e,
    S_PROC_ID_END    = 0x114f,
    S_FRAMEPROC      = 0x1012, // extra frame and proc information
    S_REGREL32       = 0x1111, // register relative address
    S_LOCAL          = 0x113e, // defines a local symbol in optimized code
    S_DEFRANGE       = 0x113f, // defines a single range of addresses in which symbol can be evaluated
    S_DEFRANGE_FRAMEPOINTER_REL = 0x1142, // range for stack symbol.
};

// types
enum {
    T_VOID          = 0x0003,   // void
    T_BOOL08        = 0x0030,   // 8 bit boolean
    T_CHAR          = 0x0010,   // 8 bit signed
    T_UCHAR         = 0x0020,   // 8 bit unsigned
    T_INT1          = 0x0068,   // 8 bit signed int
    T_UINT1         = 0x0069,   // 8 bit unsigned int
    T_INT2          = 0x0072,   // 16 bit signed int
    T_UINT2         = 0x0073,   // 16 bit unsigned int
    T_INT4          = 0x0074,   // 32 bit signed int
    T_UINT4         = 0x0075,   // 32 bit unsigned int
    T_INT8          = 0x0076,   // 64 bit signed int
    T_UINT8         = 0x0077,   // 64 bit unsigned int
    T_REAL32        = 0x0040,   // 32 bit real
    T_REAL64        = 0x0041,   // 64 bit real
};

enum {
    LF_NUMERIC          = 0x8000,
    LF_CHAR             = 0x8000,
    LF_SHORT            = 0x8001,
    LF_USHORT           = 0x8002,
    LF_LONG             = 0x8003,
    LF_ULONG            = 0x8004,
    LF_REAL32           = 0x8005,
    LF_REAL64           = 0x8006,
    LF_REAL80           = 0x8007,
    LF_REAL128          = 0x8008,
    LF_QUADWORD         = 0x8009,
    LF_UQUADWORD        = 0x800a,
    LF_REAL48           = 0x800b,
    LF_COMPLEX32        = 0x800c,
    LF_COMPLEX64        = 0x800d,
    LF_COMPLEX80        = 0x800e,
    LF_COMPLEX128       = 0x800f,
    LF_VARSTRING        = 0x8010,

    LF_POINTER          = 0x1002,
    LF_PROCEDURE        = 0x1008,
    LF_ARGLIST          = 0x1201,
    LF_FIELDLIST        = 0x1203,
    LF_ARRAY            = 0x1503,
    LF_CLASS            = 0x1504,
    LF_STRUCTURE        = 0x1505,
    LF_UNION            = 0x1506,
    LF_ENUM             = 0x1507,
    LF_MEMBER           = 0x150d,
    LF_FUNC_ID          = 0x1601,

    LF_STRING           = 0x0082,

    // the idea is that if you need to pad a
    // type you fill in the remaining space with a
    // sequence of LF_PADs like this
    //
    // Your record's bytes:
    //   DATA LF_PAD2 LF_PAD1 LF_PAD0
    LF_PAD0             = 0x00f0,
};
