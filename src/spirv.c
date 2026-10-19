/* spirv.c - minimal SPIR-V 1.0 module builder.
 *
 * Sections are accumulated separately and concatenated in the order the
 * spec requires.  Non-aggregate types and constants are deduplicated.
 * Functions are written to a side buffer: OpVariables of the current
 * function go to `fvars` (they must come first in the entry block) and
 * everything else to `fbody`; spv_end_function splices them together. */

/* opcodes */
enum {
    SpvOpName = 5, SpvOpMemberName = 6, SpvOpExtInstImport = 11, SpvOpExtInst = 12, SpvOpMemoryModel = 14,
    SpvOpEntryPoint = 15, SpvOpExecutionMode = 16, SpvOpCapability = 17,
    SpvOpTypeVoid = 19, SpvOpTypeBool = 20, SpvOpTypeInt = 21, SpvOpTypeFloat = 22, SpvOpTypeVector = 23,
    SpvOpTypeArray = 28, SpvOpTypeRuntimeArray = 29, SpvOpTypeStruct = 30, SpvOpTypePointer = 32, SpvOpTypeFunction = 33,
    SpvOpConstantTrue = 41, SpvOpConstantFalse = 42, SpvOpConstant = 43, SpvOpConstantComposite = 44,
    SpvOpConstantNull = 46, SpvOpFunction = 54, SpvOpFunctionParameter = 55, SpvOpFunctionEnd = 56,
    SpvOpFunctionCall = 57, SpvOpVariable = 59, SpvOpLoad = 61, SpvOpStore = 62, SpvOpAccessChain = 65,
    SpvOpDecorate = 71, SpvOpMemberDecorate = 72, SpvOpVectorExtractDynamic = 77, SpvOpVectorShuffle = 79,
    SpvOpCompositeConstruct = 80, SpvOpCompositeExtract = 81, SpvOpCompositeInsert = 82,
    SpvOpConvertFToU = 109, SpvOpConvertFToS = 110, SpvOpConvertSToF = 111, SpvOpBitcast = 124, SpvOpSNegate = 126, SpvOpFNegate = 127,
    SpvOpIAdd = 128, SpvOpFAdd = 129, SpvOpISub = 130, SpvOpFSub = 131, SpvOpIMul = 132, SpvOpFMul = 133,
    SpvOpSDiv = 135, SpvOpFDiv = 136, SpvOpSMod = 139, SpvOpFMod = 141, SpvOpVectorTimesScalar = 142,
    SpvOpDot = 148, SpvOpIsNan = 156, SpvOpIsInf = 157,
    SpvOpLogicalEqual = 164, SpvOpLogicalNotEqual = 165, SpvOpLogicalOr = 166, SpvOpLogicalAnd = 167,
    SpvOpLogicalNot = 168, SpvOpSelect = 169, SpvOpIEqual = 170, SpvOpINotEqual = 171,
    SpvOpSGreaterThan = 173, SpvOpSGreaterThanEqual = 175, SpvOpSLessThan = 177, SpvOpSLessThanEqual = 179,
    SpvOpFOrdEqual = 180, SpvOpFOrdNotEqual = 182, SpvOpFOrdLessThan = 184, SpvOpFOrdGreaterThan = 186,
    SpvOpFOrdLessThanEqual = 188, SpvOpFOrdGreaterThanEqual = 190,
    SpvOpLoopMerge = 246, SpvOpSelectionMerge = 247, SpvOpLabel = 248, SpvOpBranch = 249,
    SpvOpBranchConditional = 250, SpvOpReturn = 253, SpvOpReturnValue = 254, SpvOpUnreachable = 255,
};
/* storage classes, decorations, builtins */
enum { SpvSCInput = 1, SpvSCUniform = 2, SpvSCOutput = 3, SpvSCPrivate = 6, SpvSCFunction = 7, SpvSCPushConstant = 9 };
enum { SpvDecBlock = 2, SpvDecBufferBlock = 3, SpvDecArrayStride = 6, SpvDecBuiltIn = 11, SpvDecNonWritable = 24,
       SpvDecLocation = 30, SpvDecBinding = 33, SpvDecDescriptorSet = 34, SpvDecOffset = 35 };
enum { SpvBuiltInFragCoord = 15, SpvBuiltInGlobalInvocationId = 28 };
enum { SpvOpShiftRightLogical = 194, SpvOpShiftLeftLogical = 196, SpvOpBitwiseOr = 197, SpvOpBitwiseAnd = 199 };
/* GLSL.std.450 */
enum {
    GLSLTrunc = 3, GLSLFAbs = 4, GLSLSAbs = 5, GLSLFSign = 6, GLSLSSign = 7, GLSLFloor = 8, GLSLCeil = 9,
    GLSLFract = 10, GLSLSin = 13, GLSLCos = 14, GLSLTan = 15, GLSLAsin = 16, GLSLAcos = 17, GLSLAtan = 18,
    GLSLSinh = 19, GLSLCosh = 20, GLSLTanh = 21, GLSLAsinh = 22, GLSLAcosh = 23, GLSLAtanh = 24,
    GLSLAtan2 = 25, GLSLPow = 26, GLSLExp = 27, GLSLLog = 28, GLSLExp2 = 29, GLSLLog2 = 30, GLSLSqrt = 31,
    GLSLFMin = 37, GLSLSMin = 39, GLSLFMax = 40, GLSLSMax = 42, GLSLFClamp = 43, GLSLSClamp = 45, GLSLFMix = 46,
    GLSLStep = 48, GLSLSmoothStep = 49, GLSLFma = 50, GLSLLength = 66,
};

typedef VEC(u32) WordVec;

typedef struct SpvKey {
    int off, n; /* instruction words (opcode word excluded, result id slot zeroed) in `keys` */
    u32 id;
} SpvKey;

typedef struct Spv {
    u32 bound;
    WordVec caps, ext, header2, entry, exec, debug, annot, types, funcs;
    WordVec fvars, fbody;
    WordVec keywords;
    VEC(SpvKey) keys;
    u32 glsl;
} Spv;

static void wv_inst(WordVec *v, u32 op, const u32 *ops, int n) {
    vec_push(*v, ((u32)(n + 1) << 16) | op);
    for (int i = 0; i < n; i++) vec_push(*v, ops[i]);
}

static u32 spv_new_id(Spv *s) { return s->bound++; }

static int spv_string_words(const char *str, u32 *out, int cap) {
    int len = (int)strlen(str);
    int n = len / 4 + 1;
    if (n > cap) n = cap;
    memset(out, 0, sizeof(u32) * (size_t)n);
    memcpy(out, str, (size_t)FS_MIN(len, n * 4 - 1));
    return n;
}

static void spv_init(Spv *s) {
    memset(s, 0, sizeof *s);
    s->bound = 1;
    u32 cap = 1; /* Shader */
    wv_inst(&s->caps, SpvOpCapability, &cap, 1);
    s->glsl = spv_new_id(s);
    u32 ops[8];
    ops[0] = s->glsl;
    int n = spv_string_words("GLSL.std.450", ops + 1, 7);
    wv_inst(&s->ext, SpvOpExtInstImport, ops, n + 1);
    u32 mm[2] = {0, 1}; /* Logical, GLSL450 */
    wv_inst(&s->header2, SpvOpMemoryModel, mm, 2);
}

static void spv_free(Spv *s) {
    WordVec *vs[] = {&s->caps, &s->ext, &s->header2, &s->entry, &s->exec, &s->debug, &s->annot, &s->types,
                     &s->funcs, &s->fvars, &s->fbody, &s->keywords};
    for (int i = 0; i < ARRAY_COUNT(vs); i++) vec_free(*vs[i]);
    vec_free(s->keys);
}

/* deduplicated type/constant: `ops` contains the result id slot at index `rpos` (ignored for matching) */
static u32 spv_dedup(Spv *s, u32 op, u32 *ops, int n, int rpos) {
    for (int k = 0; k < s->keys.len; k++) {
        SpvKey *key = &s->keys.data[k];
        if (key->n != n + 1) continue;
        u32 *w = s->keywords.data + key->off;
        if (w[0] != op) continue;
        bool same = true;
        for (int i = 0; i < n && same; i++)
            if (i != rpos && w[1 + i] != ops[i]) same = false;
        if (same) return key->id;
    }
    u32 id = spv_new_id(s);
    ops[rpos] = id;
    SpvKey key = {s->keywords.len, n + 1, id};
    vec_push(s->keywords, op);
    for (int i = 0; i < n; i++) vec_push(s->keywords, ops[i]);
    vec_push(s->keys, key);
    wv_inst(&s->types, op, ops, n);
    return id;
}

static u32 spv_type_void(Spv *s) { u32 o[1] = {0}; return spv_dedup(s, SpvOpTypeVoid, o, 1, 0); }
static u32 spv_type_bool(Spv *s) { u32 o[1] = {0}; return spv_dedup(s, SpvOpTypeBool, o, 1, 0); }
static u32 spv_type_int(Spv *s, bool sign) { u32 o[3] = {0, 32, sign}; return spv_dedup(s, SpvOpTypeInt, o, 3, 0); }
static u32 spv_type_float(Spv *s) { u32 o[2] = {0, 32}; return spv_dedup(s, SpvOpTypeFloat, o, 2, 0); }
static u32 spv_type_vec(Spv *s, u32 comp, u32 n) { u32 o[3] = {0, comp, n}; return spv_dedup(s, SpvOpTypeVector, o, 3, 0); }
static u32 spv_type_ptr(Spv *s, u32 sc, u32 t) { u32 o[3] = {0, sc, t}; return spv_dedup(s, SpvOpTypePointer, o, 3, 0); }
static u32 spv_type_fn(Spv *s, u32 ret, const u32 *params, int n) {
    u32 o[34];
    o[0] = 0;
    o[1] = ret;
    for (int i = 0; i < n && i < 32; i++) o[2 + i] = params[i];
    return spv_dedup(s, SpvOpTypeFunction, o, 2 + FS_MIN(n, 32), 0);
}

static u32 spv_const_u32(Spv *s, u32 type, u32 bits) { u32 o[3] = {type, 0, bits}; return spv_dedup(s, SpvOpConstant, o, 3, 1); }
static u32 spv_const_int(Spv *s, i32 v) { return spv_const_u32(s, spv_type_int(s, true), (u32)v); }
static u32 spv_const_float(Spv *s, float v) {
    u32 bits;
    memcpy(&bits, &v, 4);
    return spv_const_u32(s, spv_type_float(s), bits);
}
static u32 spv_const_bool(Spv *s, bool v) {
    u32 o[2] = {spv_type_bool(s), 0};
    return spv_dedup(s, v ? SpvOpConstantTrue : SpvOpConstantFalse, o, 2, 1);
}
static u32 spv_const_composite(Spv *s, u32 type, const u32 *parts, int n) {
    u32 o[260];
    o[0] = type;
    o[1] = 0;
    for (int i = 0; i < n && i < 256; i++) o[2 + i] = parts[i];
    return spv_dedup(s, SpvOpConstantComposite, o, 2 + FS_MIN(n, 256), 1);
}
static u32 spv_const_null(Spv *s, u32 type) { u32 o[2] = {type, 0}; return spv_dedup(s, SpvOpConstantNull, o, 2, 1); }

/* aggregate types are never deduplicated by content here (callers cache them) */
static u32 spv_type_array(Spv *s, u32 elem, int n) {
    u32 id = spv_new_id(s);
    u32 o[3] = {id, elem, spv_const_int(s, n)};
    wv_inst(&s->types, SpvOpTypeArray, o, 3);
    return id;
}
static u32 spv_type_struct(Spv *s, const u32 *members, int n) {
    u32 id = spv_new_id(s);
    u32 *o = (u32 *)xmalloc(sizeof(u32) * (size_t)(n + 1));
    o[0] = id;
    if (n > 0) memcpy(o + 1, members, sizeof(u32) * (size_t)n);
    wv_inst(&s->types, SpvOpTypeStruct, o, n + 1);
    free(o);
    return id;
}

static u32 spv_global_var(Spv *s, u32 ptr_type, u32 sc, u32 init) {
    u32 id = spv_new_id(s);
    u32 o[4] = {ptr_type, id, sc, init};
    wv_inst(&s->types, SpvOpVariable, o, init ? 4 : 3);
    return id;
}

static void spv_decorate(Spv *s, u32 target, u32 dec, const u32 *lits, int n) {
    u32 o[8] = {target, dec};
    for (int i = 0; i < n && i < 6; i++) o[2 + i] = lits[i];
    wv_inst(&s->annot, SpvOpDecorate, o, 2 + n);
}
static void spv_decorate1(Spv *s, u32 target, u32 dec, u32 lit) { spv_decorate(s, target, dec, &lit, 1); }
static void spv_member_decorate(Spv *s, u32 st, u32 member, u32 dec, u32 lit, bool has_lit) {
    u32 o[4] = {st, member, dec, lit};
    wv_inst(&s->annot, SpvOpMemberDecorate, o, has_lit ? 4 : 3);
}

static void spv_name(Spv *s, u32 id, const char *name) {
    u32 o[17];
    o[0] = id;
    int n = spv_string_words(name, o + 1, 16);
    wv_inst(&s->debug, SpvOpName, o, 1 + n);
}

/* ---- function bodies ------------------------------------------------- */
static u32 spv_op(Spv *s, u32 op, u32 type, const u32 *args, int n) {
    u32 id = spv_new_id(s);
    vec_push(s->fbody, ((u32)(n + 3) << 16) | op);
    vec_push(s->fbody, type);
    vec_push(s->fbody, id);
    for (int i = 0; i < n; i++) vec_push(s->fbody, args[i]);
    return id;
}
static u32 spv_op1(Spv *s, u32 op, u32 type, u32 a) { return spv_op(s, op, type, &a, 1); }
static u32 spv_op2(Spv *s, u32 op, u32 type, u32 a, u32 b) { u32 o[2] = {a, b}; return spv_op(s, op, type, o, 2); }
static u32 spv_op3(Spv *s, u32 op, u32 type, u32 a, u32 b, u32 c) { u32 o[3] = {a, b, c}; return spv_op(s, op, type, o, 3); }
static void spv_void(Spv *s, u32 op, const u32 *args, int n) { wv_inst(&s->fbody, op, args, n); }

static u32 spv_ext(Spv *s, u32 type, u32 inst, const u32 *args, int n) {
    u32 o[8] = {s->glsl, inst};
    for (int i = 0; i < n && i < 6; i++) o[2 + i] = args[i];
    return spv_op(s, SpvOpExtInst, type, o, 2 + n);
}

static u32 spv_local_var(Spv *s, u32 ptr_type, u32 init) {
    u32 id = spv_new_id(s);
    u32 o[4] = {ptr_type, id, SpvSCFunction, init};
    wv_inst(&s->fvars, SpvOpVariable, o, init ? 4 : 3);
    return id;
}

static void spv_label(Spv *s, u32 id) { wv_inst(&s->fbody, SpvOpLabel, &id, 1); }
static void spv_branch(Spv *s, u32 target) { wv_inst(&s->fbody, SpvOpBranch, &target, 1); }
static void spv_branch_cond(Spv *s, u32 c, u32 t, u32 f) { u32 o[3] = {c, t, f}; wv_inst(&s->fbody, SpvOpBranchConditional, o, 3); }
static void spv_selection_merge(Spv *s, u32 merge) { u32 o[2] = {merge, 0}; wv_inst(&s->fbody, SpvOpSelectionMerge, o, 2); }
static void spv_loop_merge(Spv *s, u32 merge, u32 cont) { u32 o[3] = {merge, cont, 0}; wv_inst(&s->fbody, SpvOpLoopMerge, o, 3); }
static void spv_store(Spv *s, u32 ptr, u32 val) { u32 o[2] = {ptr, val}; wv_inst(&s->fbody, SpvOpStore, o, 2); }
static u32 spv_load(Spv *s, u32 type, u32 ptr) { return spv_op1(s, SpvOpLoad, type, ptr); }

/* begin a function whose id was allocated earlier; params receive fresh ids */
static void spv_begin_function(Spv *s, u32 fn, u32 ret, u32 fntype, const u32 *ptypes, u32 *pids, int np) {
    vec_clear(s->fvars);
    vec_clear(s->fbody);
    u32 o[4] = {ret, fn, 0, fntype};
    wv_inst(&s->funcs, SpvOpFunction, o, 4);
    for (int i = 0; i < np; i++) {
        pids[i] = spv_new_id(s);
        u32 p[2] = {ptypes[i], pids[i]};
        wv_inst(&s->funcs, SpvOpFunctionParameter, p, 2);
    }
    u32 entry = spv_new_id(s);
    wv_inst(&s->funcs, SpvOpLabel, &entry, 1);
}

static void spv_end_function(Spv *s) {
    for (int i = 0; i < s->fvars.len; i++) vec_push(s->funcs, s->fvars.data[i]);
    for (int i = 0; i < s->fbody.len; i++) vec_push(s->funcs, s->fbody.data[i]);
    wv_inst(&s->funcs, SpvOpFunctionEnd, NULL, 0);
    vec_clear(s->fvars);
    vec_clear(s->fbody);
}

/* GLCompute entry point "main" with LocalSize (lx, 1, 1) */
static void spv_entry_point_compute(Spv *s, u32 fn, const u32 *iface, int n, u32 lx) {
    u32 o[24];
    o[0] = 5; /* GLCompute */
    o[1] = fn;
    int k = spv_string_words("main", o + 2, 2);
    for (int i = 0; i < n && i < 16; i++) o[2 + k + i] = iface[i];
    wv_inst(&s->entry, SpvOpEntryPoint, o, 2 + k + n);
    u32 em[5] = {fn, 17 /* LocalSize */, lx, 1, 1};
    wv_inst(&s->exec, SpvOpExecutionMode, em, 5);
}

static u32 spv_type_runtime_array(Spv *s, u32 elem) {
    u32 id = spv_new_id(s);
    u32 o[2] = {id, elem};
    wv_inst(&s->types, SpvOpTypeRuntimeArray, o, 2);
    return id;
}

static void spv_entry_point(Spv *s, u32 fn, const u32 *iface, int n) {
    u32 o[24];
    o[0] = 4; /* Fragment */
    o[1] = fn;
    int k = spv_string_words("main", o + 2, 2);
    for (int i = 0; i < n && i < 16; i++) o[2 + k + i] = iface[i];
    wv_inst(&s->entry, SpvOpEntryPoint, o, 2 + k + n);
    u32 em[2] = {fn, 7}; /* OriginUpperLeft */
    wv_inst(&s->exec, SpvOpExecutionMode, em, 2);
}

/* SPV_KHR_float_controls' SignedZeroInfNanPreserve (32-bit floats) for the entry point fn: IEEE float behaviour,
   which drivers that otherwise compile with fast math (MoltenVK "on demand") respect */
static void spv_float_controls_ieee(Spv *s, u32 fn) {
    u32 cap = 4466; /* Capability SignedZeroInfNanPreserve */
    wv_inst(&s->caps, SpvOpCapability, &cap, 1);
    u32 o[8];
    int n = spv_string_words("SPV_KHR_float_controls", o, 8);
    wv_inst(&s->caps, 10 /* OpExtension */, o, n);
    u32 em[3] = {fn, 4461 /* ExecutionMode SignedZeroInfNanPreserve */, 32};
    wv_inst(&s->exec, SpvOpExecutionMode, em, 3);
}

/* final module; caller frees */
static u32 *spv_finish(Spv *s, u32 *nwords) {
    WordVec out = {0};
    u32 header[5] = {0x07230203u, 0x00010000u, 0, s->bound, 0};
    for (int i = 0; i < 5; i++) vec_push(out, header[i]);
    WordVec *parts[] = {&s->caps, &s->ext, &s->header2, &s->entry, &s->exec, &s->debug, &s->annot, &s->types, &s->funcs};
    for (int p = 0; p < ARRAY_COUNT(parts); p++)
        for (int i = 0; i < parts[p]->len; i++) vec_push(out, parts[p]->data[i]);
    out.data[3] = s->bound;
    *nwords = (u32)out.len;
    return out.data;
}
