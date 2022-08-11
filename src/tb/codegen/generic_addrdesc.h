// this is a generic code generator you can use to get started with a new register machine target in TB
// all you have to do is hook in the correct details and functions and it'll do the heavy lifting...
// it's all managed through preprocessor defines and monomorphized using #include like such:
//
//
// #define GAD_FN(name) aarch64_ ## name // all "exported" symbols have this prefix
// #define GAD_NUM_REG_FAMILIES 2
// #define GAD_REGS_IN_FAMILY 16
// #define GAD_VAL MyCpuVal
// ...
// #include "generic_addrdesc.h"
//
// Explanations:
static_assert(sizeof(float) == sizeof(uint32_t), "lil bitch... float gotta be 32bit");
static_assert(sizeof(double) == sizeof(uint64_t), "big bitch... double gotta be 64bit");

static thread_local size_t s_local_thread_id;

#define EITHER2(a, b, c)    ((a) == (b) || (a) == (c))
#define EITHER3(a, b, c, d) ((a) == (b) || (a) == (c) || (a) == (d))
#define FITS_INTO(a, type)  ((a) == ((type)(a)))

#ifndef GET_CODE_POS
#define GET_CODE_POS() (ctx->out - ctx->start_out)
#endif

enum {
    GAD_VAL_UNRESOLVED = 0,
    GAD_VAL_FLAGS      = 1,
    GAD_VAL_REGISTER   = 2,
};

// We really only need the position where to patch
// it since it's all internal and the target is implicit.
typedef uint32_t ReturnPatch;

// this would represent a set of instructions which pipe results into each other.
// it is the user's responsibility to know when the chain should be cut and this
// process is known as tiling *technically*.
typedef struct InstThread {
    TB_Reg r;
} InstThread;

typedef struct LabelPatch {
    int pos;
    TB_Label target_lbl;
} LabelPatch;

struct Ctx {
    // Header that
    uint8_t* out;
    uint8_t* start_out;

    TB_Function* f;

    // some analysis
    TB_Reg* use_count;
    int* ordinal;

    // Used to allocate stack stuff
    uint32_t stack_usage;

    // just keeps track of the callee saved registers that
    // we actually used.
    uint64_t regs_to_save;

    // Patch info
    uint32_t label_patch_count;
    uint32_t ret_patch_count;

    uint32_t* labels;
    LabelPatch* label_patches;
    ReturnPatch* ret_patches;

    // Extra stuff
    // GAD_EXTRA_CTX is a struct body
    struct GAD_EXTRA_CTX;

    // Register allocation
    TB_Reg reg_allocator[GAD_NUM_REG_FAMILIES][GAD_REGS_IN_FAMILY];
    int regs_available[GAD_NUM_REG_FAMILIES];

    // If the ISA has flags this is helpful
    TB_Reg flags_bound;
    int flags_code;

    int queue_length;
    GAD_VAL queue[];
};

typedef struct {
    size_t memory_usage;

    size_t locals_count;
    size_t return_count;
    size_t line_info_count;
    size_t label_patch_count;
} FunctionTallySimple;

static FunctionTallySimple tally_memory_usage_simple(TB_Function* restrict f) {
    size_t locals_count = 0;
    size_t return_count = 0;
    size_t label_patch_count = 0;
    size_t line_info_count = 0;

    TB_FOR_EACH_NODE(n, f) {
        TB_NodeTypeEnum t = n->type;

        if (t == TB_RET) return_count++;
        else if (t == TB_LOCAL) locals_count++;
        else if (t == TB_IF) label_patch_count += 2;
        else if (t == TB_GOTO) label_patch_count++;
        else if (t == TB_LINE_INFO) line_info_count++;
        else if (t == TB_SWITCH) {
            label_patch_count += 1 + ((n->switch_.entries_end - n->switch_.entries_start) / 2);
        }
    }

    // parameters are locals too... ish
    locals_count += f->prototype->param_count;

    size_t align_mask = _Alignof(long double) - 1;
    size_t tally = 0;

    // context
    tally += sizeof(Ctx) + (f->node_count * sizeof(GAD_VAL));
    tally = (tally + align_mask) & ~align_mask;

    // ordinal
    tally += f->node_count * sizeof(int);
    tally = (tally + align_mask) & ~align_mask;

    // use_count
    tally += f->node_count * sizeof(TB_Reg);
    tally = (tally + align_mask) & ~align_mask;

    // intervals
    tally += f->node_count * sizeof(TB_Reg);
    tally = (tally + align_mask) & ~align_mask;

    // labels
    tally += f->label_count * sizeof(uint32_t);
    tally = (tally + align_mask) & ~align_mask;

    // label_patches
    tally += label_patch_count * sizeof(LabelPatch);
    tally = (tally + align_mask) & ~align_mask;

    // ret_patches
    tally += return_count * sizeof(ReturnPatch);
    tally = (tally + align_mask) & ~align_mask;

    return (FunctionTallySimple) {
        .memory_usage = tally,
        .line_info_count = line_info_count,
        .locals_count = locals_count,
        .return_count = return_count,
        .label_patch_count = label_patch_count
    };
}

static void GAD_FN(store)(Ctx* restrict ctx, TB_Function* f, TB_Reg r);
static void GAD_FN(return)(Ctx* restrict ctx, TB_Function* f, TB_Node* restrict n);
static void GAD_FN(phi_move)(Ctx* restrict ctx, TB_Function* f, GAD_VAL* dst, TB_Reg src);
static void GAD_FN(branch_if)(Ctx* restrict ctx, TB_Function* f, TB_Reg cond, TB_Label fallthrough, TB_Label if_true, TB_Label if_false);
static void GAD_FN(cond_to_reg)(Ctx* restrict ctx, TB_Function* f, TB_Reg r, int cc);

static void GAD_FN(get_data_type_size)(TB_DataType dt, TB_CharUnits* out_size, TB_CharUnits* out_align) {
    switch (dt.type) {
        case TB_INT: {
            // round up bits to a byte
            bool is_big_int = dt.data > 64;
            int bits = is_big_int ? ((dt.data + 7) / 8) : tb_next_pow2(dt.data);

            *out_size  = ((bits+7) / 8) << dt.width;
            *out_align = is_big_int ? 8 : bits/8;
            break;
        }
        case TB_FLOAT: {
            int s = 0;
            if (dt.data == TB_FLT_32) s = 4;
            else if (dt.data == TB_FLT_64) s = 8;
            else tb_unreachable();

            *out_size = s << dt.width;
            *out_align = s;
            break;
        }
        case TB_PTR: {
            *out_size = 8;
            *out_align = 8;
            break;
        }
        default: tb_unreachable();
    }
}

static bool GAD_FN(fits_into_int32)(TB_Node* n) {
    if (n->type == TB_INTEGER_CONST &&
        n->integer.num_words == 1) {
        uint64_t x = n->integer.single_word;
        int32_t y = x & 0xFFFFFFFF;

        return (int64_t)y == x;
    }

    return false;
}

static void GAD_FN(emit_call_patches)(TB_Module* restrict m, uint32_t* restrict func_layout) {

}

static void GAD_FN(set_flags)(Ctx* restrict ctx, TB_Function* f, TB_Reg r, int cc) {
    ctx->flags_bound = r;
    ctx->flags_code = cc;
}

static void GAD_FN(kill_flags)(Ctx* restrict ctx, TB_Function* f) {
    if (ctx->flags_bound) {
        GAD_FN(cond_to_reg)(ctx, f, ctx->flags_bound, ctx->flags_code);
        ctx->flags_bound = TB_NULL_REG;
        ctx->flags_code = -1;
    }
}

static GAD_VAL GAD_FN(force_stack)(Ctx* restrict ctx, TB_Function* f, TB_Reg r, int32_t pos) {
    GAD_VAL v = GAD_MAKE_STACK_SLOT(ctx, f, r, pos);
    ctx->queue[ctx->queue_length++] = v;
    return v;
}

static GAD_VAL GAD_FN(alloc_spill)(Ctx* restrict ctx, TB_Function* f, TB_Reg r, uint32_t size, uint32_t align) {
    ctx->stack_usage = align_up(ctx->stack_usage + size, align);

    GAD_VAL v = GAD_MAKE_STACK_SLOT(ctx, f, r, -ctx->stack_usage);
    v.is_spill = true;

    ctx->queue[ctx->queue_length++] = v;
    return v;
}

static GAD_VAL GAD_FN(alloc_stack)(Ctx* restrict ctx, TB_Function* f, TB_Reg r, uint32_t size, uint32_t align) {
    ctx->stack_usage = align_up(ctx->stack_usage + size, align);

    GAD_VAL v = GAD_MAKE_STACK_SLOT(ctx, f, r, -ctx->stack_usage);
    ctx->queue[ctx->queue_length++] = v;
    return v;
}

static GAD_VAL GAD_FN(alloc_reg)(Ctx* restrict ctx, TB_Function* f, int reg_class, TB_Reg r) {
    assert(ctx->regs_available[reg_class] > 0);
    const static char priorities[GAD_NUM_REG_FAMILIES][GAD_REGS_IN_FAMILY] = GAD_REG_PRIORITIES;

    FOREACH_N(i, 0, COUNTOF(priorities[reg_class])) {
        int reg = priorities[reg_class][i];

        if (ctx->reg_allocator[reg_class][reg] == TB_NULL_REG) {
            ctx->reg_allocator[reg_class][reg] = r;
            ctx->regs_available[reg_class] -= 1;

            // mark register as to be saved
            // ctx->header.regs_to_save |= (1u << gpr) & (ctx->is_sysv ? SYSV_ABI_CALLEE_SAVED : WIN64_ABI_CALLEE_SAVED);

            return (GAD_VAL){
                .type = GAD_VAL_REGISTER + reg_class,
                .dt = r == TB_TEMP_REG ? TB_TYPE_VOID : f->nodes[r].dt,
                .r = r,
                .reg = reg
            };
        }
    }

    tb_unreachable();
    return (GAD_VAL){ 0 };
}

static void GAD_FN(reserve_register)(Ctx* restrict ctx, TB_Function* f, TB_Reg r, int reg_class, int reg_num) {
    ctx->queue[ctx->queue_length++] = (GAD_VAL){
        .type = GAD_VAL_REGISTER + reg_class,
        .dt = f->nodes[r].dt,
        .r = r,
        .reg = reg_num
    };

    ctx->reg_allocator[reg_class][reg_num] = r;
    ctx->regs_available[reg_class] -= 1;
}

static GAD_VAL GAD_FN(steal_register)(Ctx* restrict ctx, TB_Function* f, TB_DataType dt, int reg_class, int reg_num) {
    ctx->queue[ctx->queue_length++].type = GAD_VAL_UNRESOLVED;

    ctx->reg_allocator[reg_class][reg_num] = 0;
    ctx->regs_available[reg_class] += 1;

    return (GAD_VAL){
        .type = GAD_VAL_REGISTER + reg_class,
        .dt = dt,
        .reg = reg_num
    };
}

static void GAD_FN(spill_if_running_out)(Ctx* restrict ctx, TB_Function* f, TB_Reg bb, TB_Reg bb_end) {
    loop(i, GAD_NUM_REG_FAMILIES) {
        if (ctx->regs_available[i] < 4) {
            tb_todo();
        }
    }
}

// TODO(NeGate): maybe we want a lightweight hashmap instead of a queue
// returns the queue slot of r, or -1 if there's none
static ptrdiff_t GAD_FN(find)(Ctx* restrict ctx, TB_Function* f, TB_Reg r) {
    FOREACH_N(i, 0, ctx->queue_length) {
        if (ctx->queue[i].r == r) return i;
    }

    return -1;
}

// add value to the queue to be resolved once relevant
static void GAD_FN(enqueue)(Ctx* restrict ctx, TB_Function* f, TB_Reg r) {
    ptrdiff_t search = GAD_FN(find)(ctx, f, r);
    if (search >= 0) {
        printf("Reuse: r%d\n", r);
    }

    assert(ctx->queue_length < f->node_count);
    ctx->queue[ctx->queue_length++] = (GAD_VAL){
        .type = GAD_VAL_UNRESOLVED,
        .r = r
    };

    printf("Enqueue: r%d\n", r);
}

static void print_indent(int depth) {
    FOREACH_N(i, 0, depth) printf("  ");
}

// resolves all the values necessary for r, then resolves r
// returns the queue slot which holds r, -1 if there's none
static ptrdiff_t GAD_FN(await)(Ctx* restrict ctx, TB_Function* f, TB_Reg r, int depth) {
    ptrdiff_t i = GAD_FN(find)(ctx, f, r);
    if (i < 0) {
        print_indent(depth);
        printf("Not found: r%d\n", r);
        return i;
    }

    if (ctx->queue[i].type != GAD_VAL_UNRESOLVED) {
        print_indent(depth);
        printf("Readback: r%d\n", r);
        return i;
    }

    print_indent(depth);
    printf("Await: r%d\n", r);

    // we don't walk past a phi node
    if (!tb_node_is_phi_node(f, r)) {
        // resolve dependencies first
        TB_FOR_INPUT_IN_REG(it, f, r) {
            GAD_FN(await)(ctx, f, it.r, depth+1);
        }
    }

    print_indent(depth);
    printf("Resolve: r%d\n", r);

    GAD_VAL v = GAD_RESOLVE_VALUE(ctx, f, r);
    assert(v.type != GAD_VAL_UNRESOLVED);

    v.r = r;
    ctx->queue[i] = v;
    return i;
}

static GAD_VAL GAD_FN(get_val)(Ctx* restrict ctx, TB_Function* f, TB_Reg r) {
    ptrdiff_t i = GAD_FN(await)(ctx, f, r, 0);
    assert(i >= 0);

    return ctx->queue[i];
}

static void GAD_FN(eval_bb_edge)(Ctx* restrict ctx, TB_Function* f, TB_Reg from, TB_Reg to) {
    TB_Reg to_terminator = f->nodes[to].label.terminator;

    TB_FOR_EACH_NODE_RANGE(n, f, to, to_terminator) {
        TB_Reg r = n - f->nodes;

        if (tb_node_is_phi_node(f, r)) {
            TB_DataType dt = n->dt;

            int count = tb_node_get_phi_width(f, r);
            TB_PhiInput* inputs = tb_node_get_phi_inputs(f, r);

            // find if the phi node has been allocated already
            ptrdiff_t i = GAD_FN(find)(ctx, f, r);
            assert(i >= 0 && "The phi node gotta be in the queue but maybe unresolved at this point");

            if (ctx->queue[i].type == GAD_VAL_UNRESOLVED) {
                // TODO(NeGate): Fix up PHI node spill slot recycling
                TB_CharUnits size, align;
                GAD_FN(get_data_type_size)(dt, &size, &align);

                ctx->queue[i] = GAD_FN(alloc_spill)(ctx, f, r, size, size);
            }

            FOREACH_N(j, 0, count) {
                if (inputs[j].label == from) {
                    TB_Reg src = inputs[j].val;

                    if (src != TB_NULL_REG) GAD_FN(phi_move)(ctx, f, &ctx->queue[i], src);
                }
            }
        }
    }
}

static void GAD_FN(resolve_leftover)(Ctx* restrict ctx, TB_Function* f, int queue_restore_point) {
    // resolve all entries in the queue which aren't resolved yet
    FOREACH_N(it, queue_restore_point, ctx->queue_length) {
        if (ctx->queue[it].type == GAD_VAL_UNRESOLVED) {
            GAD_FN(kill_flags)(ctx, f);

            GAD_VAL v = GAD_RESOLVE_VALUE(ctx, f, ctx->queue[it].r);
            assert(v.type != GAD_VAL_UNRESOLVED);

            v.r = ctx->queue[it].r;
            ctx->queue[it] = v;
        }
    }
}

// returns the register for the next basic block
static TB_Reg GAD_FN(eval_bb)(Ctx* restrict ctx, TB_Function* f, TB_Reg bb) {
    TB_Node* start = &f->nodes[bb];
    assert(start->type == TB_LABEL);

    TB_Reg bb_end = start->label.terminator;

    // mark label
    TB_Label label_id = f->nodes[bb].label.id;
    ctx->labels[label_id] = GET_CODE_POS();

    // first node in the basic block
    TB_Reg body = f->nodes[bb].next;
    if (body == bb_end) {
        return f->nodes[bb_end].type != TB_LABEL ? f->nodes[bb_end].next : bb_end;
    }

    size_t queue_length_before = ctx->queue_length;

    TB_FOR_EACH_NODE_RANGE(n, f, body, bb_end) {
        TB_Reg r = n - f->nodes;

        TB_Node* restrict n = &f->nodes[r];
        TB_NodeTypeEnum reg_type = n->type;
        // TB_DataType dt = n->dt;

        GAD_FN(kill_flags)(ctx, f);

        // spilling
        GAD_FN(spill_if_running_out)(ctx, f, bb, bb_end);

        switch (reg_type) {
            case TB_NULL:
            case TB_PARAM:
            case TB_LOCAL:
            case TB_PARAM_ADDR:
            case TB_PHI1:
            case TB_PHI2:
            case TB_PHIN:
            break;

            case TB_LINE_INFO: {
                f->lines[f->line_count++] = (TB_Line) {
                    .file = n->line_info.file,
                    .line = n->line_info.line,
                    // .pos = GET_CODE_POS()
                };
                break;
            }

            case TB_STORE: {
                GAD_FN(store)(ctx, f, r);
                break;
            }

            default:
            // side effects need special handling, everything else is
            // just added to the queue and resolved at the lastest point
            if (TB_IS_NODE_SIDE_EFFECT(reg_type)) {
                tb_todo();
            }

            GAD_FN(enqueue)(ctx, f, r);
            break;
        }
    }


    // Evaluate the terminator
    TB_Node* end = &f->nodes[bb_end];
    TB_NodeTypeEnum end_type = end->type;

    switch (end_type) {
        case TB_LABEL: {
            // TODO(NeGate): place PHI values into the correct slots
            GAD_FN(resolve_leftover)(ctx, f, queue_length_before);
            GAD_FN(eval_bb_edge)(ctx, f, bb, bb_end);
            break;
        }

        case TB_RET: {
            GAD_FN(resolve_leftover)(ctx, f, queue_length_before);

            if (end->ret.value) {
                GAD_FN(return)(ctx, f, end);
            }

            // Only jump if we aren't literally about to end the function
            if (end->next != 0) {
                GAD_RET_JMP(ctx);
            }
            break;
        }

        case TB_IF: {
            TB_Label if_true = end->if_.if_true;
            TB_Label if_false = end->if_.if_false;

            // Resolve edges
            GAD_FN(eval_bb_edge)(ctx, f, bb, tb_find_reg_from_label(f, if_true));
            GAD_FN(eval_bb_edge)(ctx, f, bb, tb_find_reg_from_label(f, if_false));

            GAD_FN(resolve_leftover)(ctx, f, queue_length_before);

            TB_Label fallthrough_label = 0;
            if (end->next != 0) {
                fallthrough_label = f->nodes[end->next].label.id;
            }

            GAD_FN(branch_if)(ctx, f, end->if_.cond, fallthrough_label, if_true, if_false);
            break;
        }

        default: tb_todo();
    }

    ctx->queue_length = queue_length_before;
    return end->type != TB_LABEL ? end->next : bb_end;
}

static TB_FunctionOutput GAD_FN(compile_function)(TB_FunctionID id, TB_Function* restrict f, const TB_FeatureSet* features, uint8_t* out, size_t local_thread_id) {
    s_local_thread_id = local_thread_id;
    TB_TemporaryStorage* tls = tb_tls_allocate();

    //bool is_ctx_heap_allocated = false;
    Ctx* restrict ctx = NULL;
    {
        size_t ctx_size = sizeof(Ctx) + (f->node_count * sizeof(GAD_VAL));
        FunctionTallySimple tally = tally_memory_usage_simple(f);

        ctx = tb_tls_push(tls, ctx_size);
        *ctx = (Ctx){
            .out           = out,
            .start_out     = out,
            .labels        = tb_tls_push(tls, f->label_count * sizeof(uint32_t)),
            .label_patches = tb_tls_push(tls, tally.label_patch_count * sizeof(LabelPatch)),
            .ret_patches   = tb_tls_push(tls, tally.return_count * sizeof(ReturnPatch)),
            .ordinal       = tb_tls_push(tls, f->node_count * sizeof(int)),
            .use_count     = tb_tls_push(tls, f->node_count * sizeof(TB_Reg)),
        };

        f->line_count = 0;
        f->lines = tb_platform_arena_alloc(tally.line_info_count * sizeof(TB_Line));

        memset(ctx->queue, 0, f->node_count * sizeof(GAD_VAL));
        GAD_INITIAL_REG_ALLOC(ctx);
    }

    tb_function_print(f, tb_default_print_callback, stdout);

    // Analyze function for stack, use counts and phi nodes
    tb_function_calculate_use_count(f, ctx->use_count);

    GAD_RESOLVE_PARAMS(ctx, f);
    size_t original_stack_usage = ctx->stack_usage;

    // calculate the order of the nodes, it helps since node indices
    // don't actually tell us this especially once the optimizer has
    // taken a jab at it.
    //
    // also calculate the maximum parameter usage for a call
    int counter = 0;
    size_t caller_usage = 0;
    TB_FOR_EACH_NODE(n, f) {
        if (n->type == TB_PHI1 || n->type == TB_PHI2 || n->type == TB_PHIN) {
            // reserve a queue slot in global scope (aka not inside of a basic block)
            ctx->queue[ctx->queue_length++] = (GAD_VAL){
                .type = GAD_VAL_UNRESOLVED,
                .r = (n - f->nodes),
                .dt = n->dt
            };
        } else if (n->type == TB_PARAM_ADDR || n->type == TB_LOCAL) {
            GAD_RESOLVE_STACK_SLOT(ctx, f, n);
        } else if (EITHER3(n->type, TB_CALL, TB_ECALL, TB_VCALL)) {
            int param_usage = CALL_NODE_PARAM_COUNT(n);
            if (caller_usage < param_usage) caller_usage = param_usage;
        }

        ctx->ordinal[n - f->nodes] = counter++;
    }

    // Evaluate basic blocks
    TB_Reg bb = 1;
    do {
        // Generate instructions per BB
        bb = GAD_FN(eval_bb)(ctx, f, bb);
    } while (bb != TB_NULL_REG);

    // TODO(NeGate): resolve function-level patches (returns and labels)

    // hack to make the first line in a function think it's at
    // the top of the prologue not within the body
    if (f->line_count > 0) {
        f->lines[0].pos = 0;
    }

    if (ctx->stack_usage == original_stack_usage) {
        ctx->stack_usage = 0;
    }

    // we're done, clean up
    TB_FunctionOutput func_out = {
        .linkage = f->linkage,
        .code = ctx->start_out,
        .code_size = ctx->out - ctx->start_out,
        .stack_usage = ctx->stack_usage,
        .prologue_epilogue_metadata = ctx->regs_to_save
    };

    return func_out;
}
