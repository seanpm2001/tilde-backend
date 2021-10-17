#include "tb_x86_64.h"

typedef enum X64_Mod {
	X64_MOD_INDIRECT = 0,        // [rax]
	X64_MOD_INDIRECT_DISP8 = 1,  // [rax + disp8]
	X64_MOD_INDIRECT_DISP32 = 2, // [rax + disp32]
	X64_MOD_DIRECT = 3,          // rax
} X64_Mod;

typedef enum X64_ValueType {
	X64_NONE,
    
    // Real encodable types
    X64_VALUE_IMM32,
    X64_VALUE_MEM,
    X64_VALUE_GPR,
    X64_VALUE_XMM,
    
    X64_VALUE_FLAGS,
    X64_VALUE_GPR_PAIR,
} X64_ValueType;

typedef enum X64_Scale {
	X64_SCALE_X1,
	X64_SCALE_X2,
	X64_SCALE_X4,
	X64_SCALE_X8
} X64_Scale;

typedef struct X64_Value {
	X64_ValueType type : 8;
	TB_DataType dt;
    
	union {
		X64_GPR gpr : 8;
        X64_XMM xmm : 8;
        X64_Cond cond : 8;
        struct {
            X64_GPR hi, lo;
        } gpr_pair;
		struct {
			X64_GPR base : 8;
			X64_GPR index : 8;
			X64_Scale scale : 8;
			int32_t disp;
		} mem;
        int32_t imm32;
	};
} X64_Value;

typedef struct X64_LabelPatch {
	int base;
	int pos;
    TB_Label target_lbl;
} X64_LabelPatch;

typedef struct X64_PhiValue {
	TB_Register reg;
	TB_Register storage_a;
	TB_Register storage_b;
	X64_Value value;
} X64_PhiValue;

typedef struct X64_RegisterDesc {
	TB_Register bound_value;
} X64_RegisterDesc;

typedef struct X64_LocalDesc {
	TB_Register address;
    int32_t disp;
} X64_LocalDesc;

typedef struct X64_Context {
	size_t phi_count, locals_count;
    size_t cached_value_start, cached_value_end;
    
	size_t* intervals;
	X64_PhiValue* phis;
	X64_LocalDesc* locals;
    
	X64_RegisterDesc gpr_desc[16];
	X64_RegisterDesc xmm_desc[16];
} X64_Context;

typedef enum X64_InstType {
    // Integer data processing
	X64_ADD, X64_AND, X64_SUB, X64_XOR, X64_CMP, X64_MOV,
    X64_LEA, X64_IMUL, X64_MOVSX, X64_MOVZX,
    
    // Single Scalar
    X64_MOVSS, X64_ADDSS, X64_MULSS, X64_SUBSS, X64_DIVSS,
    X64_CMPSS
} X64_InstType;

typedef enum X64_ExtMode {
    // Normal
	X64_EXT_NONE,
    
    // DEF instructions have a 0F prefix
	X64_EXT_DEF,
    
    // SSE instructions have a F3 0F prefix
    X64_EXT_SSE,
} X64_ExtMode;

typedef struct X64_NormalInst {
	uint8_t op;
    
	// IMMEDIATES
	uint8_t op_i;
	uint8_t rx_i;
    
    X64_ExtMode ext : 8;
} X64_NormalInst;

static const X64_NormalInst insts[] = {
	[X64_ADD] = { 0x00, 0x80, 0x00 },
	[X64_AND] = { 0x20, 0x80, 0x04 },
	[X64_SUB] = { 0x2A, 0x80, 0x05 },
	[X64_XOR] = { 0x30, 0x80, 0x06 },
	[X64_CMP] = { 0x38, 0x80, 0x07 },
	[X64_MOV] = { 0x88, 0xC6, 0x00 },
    
	[X64_LEA] = { 0x8C },
    
	[X64_IMUL] = { 0xAE, .ext = X64_EXT_DEF },
	[X64_MOVSX] = { 0xBE, .ext = X64_EXT_DEF },
	[X64_MOVZX] = { 0xB6, .ext = X64_EXT_DEF },
    
	[X64_MOVSS] = { 0x10, .ext = X64_EXT_SSE },
	[X64_ADDSS] = { 0x58, .ext = X64_EXT_SSE },
	[X64_MULSS] = { 0x59, .ext = X64_EXT_SSE },
	[X64_SUBSS] = { 0x5C, .ext = X64_EXT_SSE },
	[X64_DIVSS] = { 0x5E, .ext = X64_EXT_SSE },
	[X64_CMPSS] = { 0xC2, .ext = X64_EXT_SSE }
};

typedef struct X64_ISel_Pattern {
	int cost;
    
	const char* pattern;
	const uint8_t* fmt;
    
	// Recycle patterns only use 2 ops, a is ignored because
	// it's aliased with dst.
	bool recycle;
	bool forced_64bit;
} X64_ISel_Pattern;

#if 0
static const X64_GPR GPR_PARAMETERS[] = {
	X64_RCX, X64_RDX, X64_R8, X64_R9
};
#endif

static const X64_GPR GPR_PRIORITY_LIST[] = {
	X64_RAX, X64_RCX, X64_RDX, X64_R8,
	X64_R9, X64_R10, X64_R11, X64_RDI,
	X64_RSI, X64_RBX, X64_R12, X64_R13,
	X64_R14, X64_R15
};

// Preprocessing stuff
static void x64_create_phi_lookup(TB_Function* f, X64_Context* ctx);
static int32_t x64_allocate_locals(TB_Function* f, X64_Context* ctx);

// IR -> Machine IR Lookups
static X64_PhiValue* x64_find_phi(X64_Context* ctx, TB_Register r);
static X64_PhiValue* x64_find_phi_values(X64_Context* ctx, TB_Register r);
static int32_t x64_find_local(X64_Context* ctx, TB_Register r);

// Machine code generation
static X64_Value x64_eval(TB_Function* f, X64_Context* ctx, TB_Emitter* out, TB_Register r, TB_Register next);
static X64_Value x64_eval_immediate(TB_Function* f, X64_Context* ctx, TB_Emitter* out, TB_Register r, const TB_Int128* imm);
static X64_Value x64_as_memory_operand(TB_Function* f, X64_Context* ctx, TB_Emitter* out, TB_DataType dt, TB_Register r);
static X64_Value x64_std_isel(TB_Function* f, X64_Context* ctx, TB_Emitter* out, TB_Register dst_reg, TB_Register a_reg, TB_Register b_reg, TB_Register next_reg, const X64_ISel_Pattern patterns[], size_t pattern_count);

// x64 instruction emitter
static void x64_inst_mov_ri64(TB_Emitter* out, X64_GPR dst, uint64_t imm);
static void x64_inst_op(TB_Emitter* out, int dt_type, const X64_NormalInst* inst, const X64_Value* a, const X64_Value* b);
static void x64_micro_assemble(TB_Emitter* out, int dt_type, const uint8_t* format, const X64_Value* operands);
static void x64_inst_nop(TB_Emitter* out, int count);

#define x64_emit_normal(out, dt, op, a, b) x64_inst_op(out, dt, &insts[X64_ ## op], a, b)
#define x64_emit_normal8(out, op, a, b) x64_inst_op(out, TB_I8, &insts[X64_ ## op], a, b)
#define x64_emit_normal16(out, op, a, b) x64_inst_op(out, TB_I16, &insts[X64_ ## op], a, b)
#define x64_emit_normal32(out, op, a, b) x64_inst_op(out, TB_I32, &insts[X64_ ## op], a, b)
#define x64_emit_normal64(out, op, a, b) x64_inst_op(out, TB_I64, &insts[X64_ ## op], a, b)

// Register allocation
static X64_Value x64_allocate_gpr(X64_Context* ctx, TB_Register reg, TB_DataType dt);
static X64_Value x64_allocate_gpr_pair(X64_Context* ctx, TB_Register reg, TB_DataType dt);
static void x64_free_gpr(X64_Context* ctx, uint8_t gpr);

TB_FunctionOutput x64_compile_function(TB_Function* f, const TB_FeatureSet* features) {
    TB_TemporaryStorage* tls = tb_tls_allocate();
    
    tb_function_print(f);
    
    // Allocate all the TLS memory for the function
	X64_Context* ctx;
	uint32_t* ret_patches;
	uint32_t* labels;
	X64_LabelPatch* label_patches;
    
	uint32_t ret_patch_count = 0;
	uint32_t label_patch_count = 0;
    {
		size_t phi_count = 0;
		size_t locals_count = 0;
		uint32_t ir_return_count = 0;
		uint32_t ir_label_count = 0;
		uint32_t ir_label_patch_count = 0;
        
		for (size_t i = 1; i < f->count; i++) {
			// Not counting the PHI1 because they aren't real PHI nodes
			if (f->nodes[i].type == TB_PHI2) phi_count++;
			else if (f->nodes[i].type == TB_LOCAL) locals_count++;
			else if (f->nodes[i].type == TB_RET) ir_return_count++;
			else if (f->nodes[i].type == TB_LABEL) ir_label_count++;
			else if (f->nodes[i].type == TB_IF) ir_label_patch_count += 2;
			else if (f->nodes[i].type == TB_GOTO) ir_label_patch_count++;
		}
        
		ctx = (X64_Context*)tb_tls_push(tls, sizeof(X64_Context) + (phi_count * sizeof(X64_PhiValue)));
        memset(ctx, 0, sizeof(X64_Context));
		
        ctx->intervals = tb_tls_push(tls, f->count * sizeof(size_t));
        ctx->phis = tb_tls_push(tls, phi_count * sizeof(size_t));
        ctx->locals = tb_tls_push(tls, locals_count * sizeof(X64_LocalDesc));
        
        ret_patches = (uint32_t*)tb_tls_push(tls, ir_return_count * sizeof(uint32_t));
		labels = (uint32_t*)tb_tls_push(tls, ir_label_count * sizeof(uint32_t));
		label_patches = (X64_LabelPatch*)tb_tls_push(tls, ir_label_patch_count * sizeof(X64_LabelPatch));
    }
    
	tb_find_live_intervals(ctx->intervals, f);
    x64_create_phi_lookup(f, ctx);
    int32_t local_stack_usage = x64_allocate_locals(f, ctx);
    
    // Reserve stack
	ctx->gpr_desc[X64_RSP].bound_value = SIZE_MAX; // reserved
	if (!features->x64.omit_frame_pointer) ctx->gpr_desc[X64_RBP].bound_value = SIZE_MAX; // reserved
    
    TB_Emitter out = { 0 };
    
    // Emit prologue
    // NOTE(NeGate): it might not be used but we assume
    // it will and if not we can just skip it in the file
    // output
    size_t prologue_patch = out.count + 3;
    {
        // sub rsp, 0 # patched later
        uint8_t* out_buffer = tb_out_reserve(&out, 7);
        out_buffer[0] = x64_inst_rex(true, 0x00, X64_RSP, 0);
        out_buffer[1] = 0x81;
        out_buffer[2] = x64_inst_mod_rx_rm(X64_MOD_DIRECT, 0x00, X64_RSP);
        *((uint32_t*)&out_buffer[3]) = 0;
        tb_out_commit(&out, 7);
    }
    
    // Go through each basic block:
    // Generate instructions from the side-effect nodes using
    // all the other nodes and then terminate the basic block
    TB_Register bb = 1; // The initial label is always at r1
    do {
        assert(f->nodes[bb].type == TB_LABEL);
        TB_Label label_id = f->nodes[bb].label.id;
        TB_Register bb_end = f->nodes[bb].label.terminator;
        
        // Clear and initialize new cache
        printf("Process BB: r%llu-r%llu\n", bb, bb_end);
        labels[label_id] = out.count;
        
        // Evaluate all side effect instructions
        // Don't eval if the basic block is empty
        if (bb != bb_end) loop_range(i, bb + 1, bb_end) {
            TB_DataType dt = f->nodes[i].dt;
            
            switch (f->nodes[i].type) {
                case TB_LOCAL: // the allocation is handled beforehand
                case TB_LOAD:
                case TB_INT_CONST:
                case TB_ADD:
                case TB_SUB:
                case TB_MUL:
                case TB_SDIV:
                case TB_UDIV:
                case TB_CMP_EQ:
                case TB_CMP_NE:
                case TB_CMP_ULT:
                case TB_CMP_ULE:
                case TB_CMP_SLT:
                case TB_CMP_SLE:
                break;
                case TB_STORE: {
                    // TODO(NeGate): Allow for patterns such as:
                    // *p = *p + a  => add Xword [p], a
                    TB_Register address_reg = f->nodes[i].store.address;
                    TB_Register value_reg = f->nodes[i].store.value;
                    
                    // Eval address and cast to the correct type for the store
                    X64_Value address = x64_eval(f, ctx, &out, address_reg, i);
                    if (address.dt.type != TB_PTR) abort();
                    
                    X64_Value value = x64_eval(f, ctx, &out, value_reg, i);
                    
                    // TODO(NeGate): Cast to store type
                    
                    x64_emit_normal(&out, dt.type, MOV, &address, &value);
                    break;
                }
                default: 
                abort();
            }
        }
        
        // TODO(NeGate): Terminate any cached values
        // Handle the terminator node
        if (f->nodes[bb_end].type == TB_IF) {
            //TB_Register true_label_reg = tb_find_reg_from_label(f, f->nodes[i].if_.if_true);
            //TB_Register false_label_reg = tb_find_reg_from_label(f, f->nodes[i].if_.if_false);
            //TB_Register true_label_end = f->nodes[true_label_reg].label.terminator;
            //TB_Register false_label_end = f->nodes[false_label_reg].label.terminator;
            
            X64_Value cond = x64_eval(f, ctx, &out, f->nodes[bb_end].if_.cond, bb_end);
            
            TB_Register if_true = f->nodes[bb_end].if_.if_true;
            TB_Register if_false = f->nodes[bb_end].if_.if_false;
            
            // Must be converted into FLAGS
            assert(cond.type == X64_VALUE_FLAGS);
            
            // JCC .true
            // JMP .false # elidable if it points to the next instruction
            uint8_t* out_buffer = tb_out_reserve(&out, 11);
            out_buffer[0] = 0x0F;
            out_buffer[1] = 0x80 + (uint8_t)cond.cond;
            out_buffer[2] = 0x00;
            out_buffer[3] = 0x00;
            out_buffer[4] = 0x00;
            out_buffer[5] = 0x00;
            
            label_patches[label_patch_count++] = (X64_LabelPatch){
                .base = out.count, .pos = out.count + 2, .target_lbl = if_true
            };
            
            if (!(f->nodes[bb_end + 1].type == TB_LABEL && f->nodes[bb_end + 1].label.id == if_false)) {
                label_patches[label_patch_count++] = (X64_LabelPatch){
                    .base = out.count + 6, .pos = out.count + 5, .target_lbl = if_false
                };
                
                out_buffer[6] = 0xE9;
                out_buffer[7] = 0x00;
                out_buffer[8] = 0x00;
                out_buffer[9] = 0x00;
                out_buffer[10] = 0x00;
                
                tb_out_commit(&out, 11);
            } else {
                tb_out_commit(&out, 6);
            }
            
            bb = bb_end + 1;
        } else if (f->nodes[bb_end].type == TB_RET) {
            assert(f->nodes[bb_end].dt.type != TB_VOID);
            X64_Value value = x64_eval(f, ctx, &out, f->nodes[bb_end].ret.value, bb_end);
            
            if (value.dt.type == TB_I8 ||
                value.dt.type == TB_I16 ||
                value.dt.type == TB_I32 ||
                value.dt.type == TB_I64 ||
                value.dt.type == TB_PTR) {
                // Integer results use RAX and if result is extended RDX
                X64_Value dst = (X64_Value){
                    .type = X64_VALUE_GPR,
                    .dt = value.dt,
                    .gpr = X64_RAX
                };
                
                if (value.type != X64_VALUE_GPR || (value.type == X64_VALUE_GPR && value.gpr != X64_RAX)) {
                    x64_emit_normal(&out, value.dt.type, MOV, &dst, &value);
                }
            } else abort();
            
            // Only jump if we aren't literally about to end the function
            if (bb_end + 1 != f->count) {
                ret_patches[ret_patch_count++] = out.count + 1;
                
                uint8_t* out_buffer = tb_out_reserve(&out, 5);
                out_buffer[0] = 0xE9;
                out_buffer[1] = 0x00;
                out_buffer[2] = 0x00;
                out_buffer[3] = 0x00;
                out_buffer[4] = 0x00;
                tb_out_commit(&out, 5);
            }
            bb = bb_end + 1;
        } else if (f->nodes[bb_end].type == TB_LABEL) {
            bb = bb_end;
        } else {
            abort(); // TODO
        }
    } while (bb != f->count);
    
    // Align stack usage to 16bytes and add 8 bytes for the return address
    local_stack_usage += (16 - (local_stack_usage % 16)) % 16;
    assert((local_stack_usage & 15) == 0);
    local_stack_usage += 8;
    
    // Patch prologue (or just omit it)
    // and emit epilogue (or dont)
    if (local_stack_usage > 8) {
        // patch prologue
        *((uint32_t*)&out.data[prologue_patch]) = f->locals_stack_usage;
        
        // add rsp, stack_usage
        uint8_t* out_buffer = tb_out_reserve(&out, 7);
        out_buffer[0] = x64_inst_rex(true, 0x00, X64_RSP, 0);
        out_buffer[1] = 0x81;
        out_buffer[2] = x64_inst_mod_rx_rm(X64_MOD_DIRECT, 0x00, X64_RSP);
        *((uint32_t*)&out_buffer[3]) = local_stack_usage;
        tb_out_commit(&out, 7);
    }
    
    // patch return
    for (int i = 0; i < ret_patch_count; i++) {
        uint32_t pos = ret_patches[i];
        
        *((uint32_t*)&out.data[pos]) = out.count - (pos + 4);
    }
    
    // patch labels
    for (size_t i = 0; i < label_patch_count; i++) {
        uint32_t pos = label_patches[i].pos;
        uint32_t target_lbl = label_patches[i].target_lbl;
        
        int32_t rel32 = labels[target_lbl] - (pos + 4);
        
        *((uint32_t*)&out.data[pos]) = rel32;
    }
    
    tb_out1b(&out, 0xC3); // ret
    
    // the written size of the function taking into account the prologue skipping
    size_t actual_size = out.count;
    if (f->locals_stack_usage <= 8) actual_size -= 7; // prologue is 7 bytes long
    
    // align functions to 16bytes
    x64_inst_nop(&out, 16 - (actual_size % 16));
    
    // Trim code output memory
    out.capacity = out.count;
    out.data = realloc(out.data, out.capacity);
    if (!out.data) abort(); // I don't know if this can even fail...
    
    return (TB_FunctionOutput) {
        .name = f->name,
        .has_no_prologue = (f->locals_stack_usage <= 8),
        .emitter = out
    };
}

static X64_Value x64_eval(TB_Function* f, X64_Context* ctx, TB_Emitter* out, TB_Register r, TB_Register next) {
    TB_Node* reg = &f->nodes[r];
    //TB_DataType dt = reg->dt;
    assert(reg->dt.count == 1);
    
    switch (reg->type) {
        case TB_INT_CONST: {
            return x64_eval_immediate(f, ctx, out, r, &reg->i_const);
        }
        case TB_ADD: {
            return x64_std_isel(f, ctx, out, r, reg->i_arith.a, reg->i_arith.b, next, 
                                (X64_ISel_Pattern[]) {
                                    { 1, "rrr", (uint8_t[]){ X64_LEA, 0, '[', 1, 2, 0x7F }, false, true },
                                    { 1, "rri", (uint8_t[]){ X64_LEA, 0, '[', 1, 2, 0x7F }, false, true },
                                    { 2, "rrr", (uint8_t[]){ X64_ADD, 0, 2, 0x7F }, true },
                                    { 2, "rri", (uint8_t[]){ X64_ADD, 0, 2, 0x7F }, true },
                                    { 3, "rrm", (uint8_t[]){ X64_ADD, 0, 2, 0x7F }, true },
                                    { 4, "rrm", (uint8_t[]){ X64_MOV, 0, 1, X64_ADD, 0, 2, 0x7F }, false },
                                    { 4, "rmr", (uint8_t[]){ X64_MOV, 0, 1, X64_ADD, 0, 2, 0x7F }, false },
                                    { 4, "rmi", (uint8_t[]){ X64_MOV, 0, 1, X64_ADD, 0, 2, 0x7F }, false }
                                }, 8);
        }
        case TB_LOCAL: {
            return (X64_Value) {
                .type = X64_VALUE_MEM,
                .dt = TB_TYPE_PTR(),
                .mem = {
                    .base = X64_RSP,
                    .index = X64_GPR_NONE,
                    .scale = X64_SCALE_X1,
                    .disp = x64_find_local(ctx, r)
                }
            };
        }
        case TB_CMP_EQ:
        case TB_CMP_NE:
        case TB_CMP_SLT:
        case TB_CMP_SLE:
        case TB_CMP_ULT:
        case TB_CMP_ULE: {
            TB_DataType cmp_dt = reg->cmp.dt;
            
            X64_Value a = x64_eval(f, ctx, out, reg->cmp.a, r);
            X64_Value b = x64_eval(f, ctx, out, reg->cmp.b, r);
            
            bool invert = false;
            if (a.type == X64_VALUE_MEM && b.type == X64_VALUE_MEM) {
                X64_Value dst = x64_allocate_gpr(ctx, reg->cmp.a, cmp_dt);
                x64_emit_normal(out, cmp_dt.type, MOV, &dst, &a);
                x64_emit_normal(out, cmp_dt.type, CMP, &dst, &b);
                x64_free_gpr(ctx, dst.gpr);
            }
            else {
                invert = (a.type == X64_VALUE_IMM32);
                
                if (invert) x64_emit_normal(out, cmp_dt.type, CMP, &b, &a);
                else x64_emit_normal(out, cmp_dt.type, CMP, &a, &b);
            }
            
            X64_Cond cc;
            switch (reg->type) {
                case TB_CMP_EQ: cc = X64_E; break;
                case TB_CMP_NE: cc = X64_NE; break;
                case TB_CMP_SLT: cc = X64_L; break;
                case TB_CMP_SLE: cc = X64_LE; break;
                case TB_CMP_ULT: cc = X64_B; break;
                case TB_CMP_ULE: cc = X64_BE; break;
                default: abort();
            }
            
            if (invert) {
                if (cc & 1) cc &= ~1;
                else cc |= 1;
            }
            
            // TODO(NeGate): Implement the case where the value is converted 
            // into a byte, IF nodes don't require it but it may come up in 
            // code.
            assert(f->nodes[next].type == TB_IF);
            return (X64_Value) { .type = X64_VALUE_FLAGS, .cond = cc };
        }
        case TB_LOAD: {
            X64_Value addr = x64_eval(f, ctx, out, reg->load.address, r);
            
            if (f->nodes[reg->load.address].type == TB_LOCAL ||
                f->nodes[reg->load.address].type == TB_PARAM_ADDR) {
                return addr;
            }
            else {
                X64_Value dst = x64_allocate_gpr(ctx, r, reg->dt);
                x64_emit_normal(out, reg->dt.type, MOV, &dst, &addr);
                return dst;
            }
        }
        default: abort();
    }
}

static X64_Value x64_eval_immediate(TB_Function* f, X64_Context* ctx, TB_Emitter* out, TB_Register r, const TB_Int128* imm) {
    TB_DataType dt = f->nodes[r].dt;
    
    // x64 can only handle 32bit immediates within
    // normal instructions, if you want bigger an
    // explicit MOV is required and 128bit immediates
    // require 2 registers.
    if (imm->hi) {
        // register pair
        assert(dt.type == TB_I128);
        
        X64_Value pair = x64_allocate_gpr_pair(ctx, r, dt);
        x64_inst_mov_ri64(out, pair.gpr_pair.lo, imm->lo);
        x64_inst_mov_ri64(out, pair.gpr_pair.hi, imm->hi);
        return pair;
    } else if (imm->lo > UINT32_MAX) {
        // explicit mov
        assert(dt.type == TB_I64 || dt.type == TB_PTR || dt.type == TB_I128);
        
        X64_Value dst = x64_allocate_gpr(ctx, r, dt);
        x64_inst_mov_ri64(out, dst.gpr, imm->lo);
        return dst;
    }
    
    // 32bit immediate case
    return (X64_Value) {
        .type = X64_VALUE_IMM32,
        .dt = dt,
        .imm32 = imm->lo
    };
}

static X64_Value x64_allocate_gpr(X64_Context* ctx, TB_Register reg, TB_DataType dt) {
	for (unsigned int i = 0; i < 14; i++) {
		X64_GPR gpr = GPR_PRIORITY_LIST[i];
        
		if (ctx->gpr_desc[gpr].bound_value == 0) {
			ctx->gpr_desc[gpr].bound_value = reg;
            
			return (X64_Value) {
				.type = X64_VALUE_GPR,
                .dt = dt,
                .gpr = gpr
			};
		}
	}
    
	// Spill GPRs
	abort();
}

static X64_Value x64_allocate_gpr_pair(X64_Context* ctx, TB_Register reg, TB_DataType dt) {
	X64_Value lo = x64_allocate_gpr(ctx, reg, TB_TYPE_I64(1));
	X64_Value hi = x64_allocate_gpr(ctx, reg, TB_TYPE_I64(1));
    
	return (X64_Value) {
		.type = X64_VALUE_GPR_PAIR,
        .dt = dt,
        .gpr_pair = { lo.gpr, hi.gpr }
	};
}

static void x64_free_gpr(X64_Context* ctx, uint8_t gpr) {
	ctx->gpr_desc[gpr].bound_value = 0;
}

static void x64_inst_mov_ri64(TB_Emitter* out, X64_GPR dst, uint64_t imm) {
    uint8_t* out_buffer = tb_out_reserve(out, 10);
    
    *out_buffer++ = x64_inst_rex(true, 0x0, dst, 0);
    *out_buffer++ = 0xB8 + (dst & 0b111);
    
    *((uint64_t*)out_buffer) = imm;
    out_buffer += 8;
    
    tb_out_commit(out, 10);
}

// NOTE(NeGate): Both arguments cannot be memory operands
void x64_inst_op(TB_Emitter* out, int dt_type, const X64_NormalInst* inst, const X64_Value* a, const X64_Value* b) {
	// x64 can only have up to 16bytes in one instruction
    uint8_t* out_buffer_start = tb_out_reserve(out, 16);
    uint8_t* out_buffer = out_buffer_start;
    
    bool dir = (b->type == X64_VALUE_MEM);
	if (dir) tb_swap(a, b);
    
	// operand size
	uint8_t sz = (dt_type != TB_I8);
    
    // All instructions that go through here are
    // based on the ModRxRm encoding so we do need
    // an RX and an RM (base, index, shift, disp)
    uint8_t base = 0;
    uint8_t rx = GPR_NONE;
    if (inst->ext == X64_EXT_NONE || inst->ext == X64_EXT_DEF) {
        assert(dt_type == TB_I8 || dt_type == TB_I16 || dt_type == TB_I32 || dt_type == TB_I64 || dt_type == TB_PTR);
        
        // Address size prefix
        if (dt_type == TB_I8 || dt_type == TB_I16) {
            *out_buffer++ = 0x66;
        }
        
        // RX
        if (b->type == X64_VALUE_GPR) rx = b->gpr;
        else if (b->type == X64_VALUE_IMM32) rx = inst->rx_i;
        else __builtin_unreachable();
        
        // RM & REX
        bool is_64bit = (dt_type == TB_I64 || dt_type == TB_PTR);
        
        if (a->type == X64_VALUE_GPR) {
            base = a->gpr;
            
            if (a->gpr >= 8 || is_64bit) {
                *out_buffer++ = x64_inst_rex(is_64bit, rx, a->gpr, 0);
            }
        }
        else if (a->type == X64_VALUE_MEM) {
            base = a->mem.base;
            
            uint8_t rex_index = (a->mem.index != X64_GPR_NONE ? a->mem.index : 0);
            if (a->mem.base >= 8 || rex_index >= 8 || is_64bit) {
                *out_buffer++ = x64_inst_rex(is_64bit, rx, a->mem.base, rex_index);
            }
        }
        else __builtin_unreachable();
        
        // Opcode
        if (inst->ext == X64_EXT_DEF) {
            // DEF instructions can only be 32bit and 64bit
            assert(dt_type == TB_I32 || dt_type == TB_I64 || dt_type == TB_PTR);
            *out_buffer++ = 0x0F;
        }
        
        if (b->type == X64_VALUE_IMM32 && inst->op_i == 0 && inst->rx_i == 0) {
            // No immediate version
            __builtin_unreachable();
        }
        
        // Immediates have a custom opcode
        uint8_t op = b->type == X64_VALUE_IMM32 ? inst->op_i : inst->op;
        *out_buffer++ = op | sz | (dir ? 2 : 0);
    }
    else if (inst->ext == X64_EXT_SSE) {
        assert(b->type != X64_VALUE_IMM32);
        assert(dt_type == TB_F32 || dt_type == TB_F64);
        
        // REX
        if (a->xmm >= 8) {
            *out_buffer++ = x64_inst_rex(true, rx, a->xmm, 0);
        }
        
        *out_buffer++ = 0xF3;
        *out_buffer++ = 0x0F;
        
        *out_buffer++ = inst->op | sz | (dir ? 2 : 0);
    }
    else __builtin_unreachable();
    
    // We forgot a case!
    assert(rx != GPR_NONE);
    
    // Operand encoding
    if (a->type == X64_VALUE_GPR || a->type == X64_VALUE_XMM) {
        *out_buffer++ = x64_inst_mod_rx_rm(X64_MOD_DIRECT, rx, base);
    } else if (a->type == X64_VALUE_MEM) {
        uint8_t index = a->mem.index;
        uint8_t scale = a->mem.scale;
        int32_t disp = a->mem.disp;
        
		bool needs_index = (index != GPR_NONE) || (base & 7) == X64_RSP;
		
        // If it needs an index, it'll put RSP into the base slot
        // and write the real base into the SIB
        uint8_t mod = X64_MOD_INDIRECT_DISP32;
        if (disp == 0) mod = X64_MOD_INDIRECT_DISP8;
        else if (disp == (int8_t)disp) mod = X64_MOD_INDIRECT_DISP8;
        
        *out_buffer++ = x64_inst_mod_rx_rm(mod, rx, needs_index ? X64_RSP : base);
        if (needs_index) {
            *out_buffer++ = x64_inst_mod_rx_rm(scale, (base & 7) == X64_RSP ? X64_RSP : index, base);
        }
        
		if (mod == X64_MOD_INDIRECT_DISP8) {
			*out_buffer++ = (int8_t)disp;
        }
		else if (mod == X64_MOD_INDIRECT_DISP32) {
            *((uint32_t*)out_buffer) = disp;
            out_buffer += 4;
		}
    } else __builtin_unreachable();
    
    if (b->type == X64_VALUE_IMM32) {
        // TODO(NeGate): Implement short immediate operands
        assert(dt_type == TB_I32 || dt_type == TB_I64 || dt_type == TB_PTR);
        
        *((uint32_t*)out_buffer) = b->imm32;
        out_buffer += 4;
    }
    
    tb_out_commit(out, out_buffer - out_buffer_start);
}

static void x64_inst_nop(TB_Emitter* out, int count) {
	if (count == 0) return;
    
	/*
	NOPS lol
	90H                             nop
	66 90H                          data16 nop
	0F 1F 00H                       nop dword [rax]
	0F 1F 40 00H                    nop dword [rax + 0x00]
	0F 1F 44 00 00H                 nop dword [rax + rax + 0x00]
	66 0F 1F 44 00 00H              nop  word [rax + rax + 0x00]
	0F 1F 80 00 00 00 00H           nop dword [rax + 0x00000000]
	0F 1F 84 00 00 00 00 00H        nop dword [rax + rax + 0x00000000]
	66 0F 1F 84 00 00 00 00 00H     nop  word [rax + rax + 0x00000000]
	66 2E 0F 1F 84 00 00 00 00 00H  nop  word cs:[rax + rax + 0x00000000]
	*/
    
	uint8_t* out_buffer = tb_out_reserve(out, count);
	do {
		if (count >= 10) {
            memcpy(out_buffer, (uint8_t[10]) { 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 }, 10);
            out_buffer += 10;
			count -= 10;
		}
        
		if (count >= 9) {
			memcpy(out_buffer, (uint8_t[9]) { 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 }, 9);
			out_buffer += 9;
            count -= 9;
		}
        
		if (count >= 8) {
			memcpy(out_buffer, (uint8_t[8]) { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 }, 8);
			out_buffer += 8;
            count -= 8;
		}
        
		if (count >= 7) {
			memcpy(out_buffer, (uint8_t[7]) { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 }, 7);
			out_buffer += 7;
            count -= 7;
		}
        
		if (count >= 6) {
			memcpy(out_buffer, (uint8_t[6]) { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 }, 6);
			out_buffer += 6;
            count -= 6;
		}
        
		if (count >= 5) {
			memcpy(out_buffer, (uint8_t[5]) { 0x0F, 0x1F, 0x44, 0x00, 0x00 }, 5);
			out_buffer += 5;
			count -= 5;
		}
        
		if (count >= 4) {
			memcpy(out_buffer, (uint8_t[4]) { 0x0F, 0x1F, 0x40, 0x00 }, 4);
			out_buffer += 4;
			count -= 4;
		}
        
		if (count >= 3) {
			memcpy(out_buffer, (uint8_t[3]) { 0x0F, 0x1F, 0x00 }, 3);
			out_buffer += 3;
			count -= 3;
		}
        
		if (count >= 2) {
			memcpy(out_buffer, (uint8_t[2]) { 0x66, 0x90 }, 2);
			out_buffer += 2;
			count -= 2;
		}
        
		if (count >= 1) {
			tb_out1b_UNSAFE(out, 0x90);
			out_buffer += 1;
			count -= 1;
		}
	} while (count);
    
    tb_out_commit(out, count);
}

// Going to be used in the instruction selector, promote 8bit and 16bit to 32bit or 64bit.
// Immediates can go either way but with GPRs and memory prefer 32bit if possible.
static X64_Value x64_legalize(TB_Function* f, X64_Context* ctx, TB_Emitter* out, TB_DataType dt, TB_Register reg, TB_Register next) {
    // TODO(NeGate): Vectors
    assert(dt.count == 1);
    X64_Value v = x64_eval(f, ctx, out, reg, next);
    
	// This is kinda weird but essentially a load might 
	// return the address because this is x64 and we don't
	// need to load some in a separate instruction.
	if (dt.type == TB_PTR && f->nodes[reg].type == TB_LOAD) {
		v.dt = dt;
	}
    
    // This only needs to worry about 8 and 16bit GPRs
	if (v.type != X64_VALUE_GPR) return v;
    if (dt.type == TB_I32 || dt.type == TB_I64 || dt.type == TB_PTR) return v;
    
    X64_Value dst = x64_allocate_gpr(ctx, reg, TB_TYPE_I32(1));
    
    // TODO(NeGate): Implement sign extend case
    __debugbreak(); // Test this case
    x64_emit_normal(out, dt.type, MOVZX, &dst, &v);
    return dst;
}

static char x64_value_type_to_pattern_char(X64_ValueType type) {
	switch (type) {
        case X64_VALUE_IMM32: return 'i';
        case X64_VALUE_GPR: return 'r';
        case X64_VALUE_XMM: return 'x';
        case X64_VALUE_MEM: return 'm';
        default: abort();
	}
}

// Not built for vector selection
// Maybe it will be one day?
// if `next_reg` is not 0, then it's the register which we expect the `dst_reg` to go into
static X64_Value x64_std_isel(TB_Function* f, X64_Context* ctx, TB_Emitter* out, TB_Register dst_reg, TB_Register a_reg, TB_Register b_reg, TB_Register next_reg, const X64_ISel_Pattern patterns[], size_t pattern_count) {
	TB_DataType dst_dt = f->nodes[dst_reg].dt;
	assert(dst_dt.count == 1);
    
	X64_Value a = x64_legalize(f, ctx, out, dst_dt, a_reg, dst_reg);
	X64_Value b = x64_legalize(f, ctx, out, dst_dt, b_reg, dst_reg);
    
	bool can_recycle = (ctx->intervals[a_reg] == dst_reg);
	if (f->nodes[next_reg].type == TB_RET &&
		a.type == X64_VALUE_GPR &&
		b.type == X64_VALUE_GPR &&
		(a.gpr != X64_RAX ||
         b.gpr != X64_RAX)) {
		// If it's about to be returned and none of 
        // the inputs are RAX, don't recycle
		can_recycle = false;
	}
    
	if (a.type == X64_VALUE_IMM32 && b.type != X64_VALUE_IMM32) tb_swap(a, b);
    
	// If both source operands are memory addresses, change one into a register
	if (a.type == X64_VALUE_MEM && b.type == X64_VALUE_MEM) {
		X64_Value temp = x64_allocate_gpr(ctx, a_reg, a.dt);
		x64_emit_normal(out, dst_dt.type, MOV, &temp, &a);
		a = temp;
	}
    
	bool will_try_recycle = can_recycle && a.type != X64_VALUE_IMM32;
    
	// Identify the pattern of the input
	char pattern_str[4] = {};
	pattern_str[0] = 'r';
	pattern_str[1] = x64_value_type_to_pattern_char(a.type);
	pattern_str[2] = x64_value_type_to_pattern_char(b.type);
	pattern_str[3] = '\0';
    
	// Find best pattern
	int lowest_cost = 255;
	const X64_ISel_Pattern* best_match = 0;
    
	for (size_t i = 0; i < pattern_count; i++) {
		int actual_cost = patterns[i].cost + (!patterns[i].recycle && will_try_recycle ? 3 : 0);
        
		if (actual_cost < lowest_cost &&
			memcmp(pattern_str, patterns[i].pattern, 4) == 0) {
			best_match = &patterns[i];
			lowest_cost = actual_cost;
		}
	}
    
	// Pattern matcher failed
	if (best_match == NULL) abort();
    
	X64_Value dst;
	if (best_match->recycle) dst = a;
	else if (f->nodes[next_reg].type == TB_RET) {
        // It doesn't matter if something was using RAX before
        // since we're about to exit
        dst = (X64_Value){ .type = X64_VALUE_GPR, .gpr = X64_RAX };
    } else dst = x64_allocate_gpr(ctx, dst_reg, dst_dt);
    
	const X64_Value operands[3] = { dst, a, b };
	x64_micro_assemble(
                       out,
                       best_match->forced_64bit ? TB_I64 : dst_dt.type,
                       best_match->fmt, operands
                       );
	return dst;
}

static const uint8_t* x64_micro_assemble_operand(const uint8_t* format, X64_Value* dst, const X64_Value* operands) {
    if (*format == '[') {
        format++;
        
        // Memory operands
        assert(operands[format[0]].type == X64_VALUE_GPR);
        assert(operands[format[1]].type == X64_VALUE_GPR);
        X64_GPR base = operands[format[0]].gpr;
        X64_GPR index = operands[format[1]].gpr;
        format += 2;
        
        *dst = (X64_Value){
            .type = X64_VALUE_MEM,
            .dt = TB_TYPE_I64(1),
            .mem = {
                .base = base,
                .index = index,
                .scale = X64_SCALE_X1,
                .disp = 0
            }
        };
    } else {
        *dst = operands[format[0]];
        format++;
    }
    
    return format;
}

static void x64_micro_assemble(TB_Emitter* out, int dt_type, const uint8_t* format, const X64_Value* operands) {
    while (*format != 0x7F) {
		X64_InstType inst = *format++;
        
        X64_Value left, right;
        format = x64_micro_assemble_operand(format, &left, operands);
        format = x64_micro_assemble_operand(format, &right, operands);
        
		x64_inst_op(out, dt_type, &insts[inst], &left, &right);
    }
}

static int32_t x64_allocate_locals(TB_Function* f, X64_Context* ctx) {
    int32_t stack_usage = 0;
    
    loop(i, f->count) if (f->nodes[i].type == TB_LOCAL) {
        uint32_t size = f->nodes[i].local.size;
        uint32_t align = f->nodes[i].local.alignment;
        
        // Increment and align
        stack_usage += size;
        stack_usage += (align - (stack_usage % align)) % align;
        
        printf("Stack alloc: %u bytes (%u align)\n", size, align);
        
        ctx->locals[ctx->locals_count++] = (X64_LocalDesc) {
            .address = i,
            .disp = -stack_usage
        };
    }
    
    return stack_usage;
}

static void x64_create_phi_lookup(TB_Function* f, X64_Context* ctx) {
    // Generate PHI lookup table
    loop(i, f->count) {
        if (f->nodes[i].type == TB_PHI1) {
            ctx->phis[ctx->phi_count++] = (X64_PhiValue){
                .reg = i, .storage_a = f->nodes[i].phi1.a
            };
        }
        else if (f->nodes[i].type == TB_PHI2) {
            ctx->phis[ctx->phi_count++] = (X64_PhiValue){
                .reg = i, .storage_a = f->nodes[i].phi2.a, .storage_b = f->nodes[i].phi2.b
            };
        }
    }
}

static int32_t x64_find_local(X64_Context* ctx, TB_Register r) {
    loop(i, ctx->locals_count) {
		if (ctx->locals[i].address == r) return ctx->locals[i].disp;
	}
    
	abort();
}

static X64_PhiValue* x64_find_phi(X64_Context* ctx, TB_Register r) {
    loop(i, ctx->phi_count) {
		if (ctx->phis[i].reg == r) return &ctx->phis[i];
	}
    
	return NULL;
}

// Searches by the values the PHI node could have
static X64_PhiValue* x64_find_phi_values(X64_Context* ctx, TB_Register r) {
    loop(i, ctx->phi_count) {
		if (ctx->phis[i].storage_a == r || ctx->phis[i].storage_b == r) return &ctx->phis[i];
	}
    
	return NULL;
}
