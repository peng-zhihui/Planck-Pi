{
	"variable-offset ctx access",
	.insns = {
	/* Get an unknown value */
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_2, 4),
	/* add it to skb.  We now have either &skb->len or
	 * &skb->pkt_type, but we don't know which
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_1, BPF_REG_2),
	/* dereference it */
	BPF_LDX_MEM(BPF_W, BPF_REG_0, BPF_REG_1, 0),
	BPF_EXIT_INSN(),
	},
	.errstr = "variable ctx access var_off=(0x0; 0x4)",
	.result = REJECT,
	.prog_type = BPF_PROG_TYPE_LWT_IN,
},
{
	"variable-offset stack access",
	.insns = {
	/* Fill the top 8 bytes of the stack */
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0),
	/* Get an unknown value */
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_2, 4),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_2, 8),
	/* add it to fp.  We now have either fp-4 or fp-8, but
	 * we don't know which
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_10),
	/* dereference it */
	BPF_LDX_MEM(BPF_W, BPF_REG_0, BPF_REG_2, 0),
	BPF_EXIT_INSN(),
	},
	.errstr = "variable stack access var_off=(0xfffffffffffffff8; 0x4)",
	.result = REJECT,
	.prog_type = BPF_PROG_TYPE_LWT_IN,
},
{
	"indirect variable-offset stack access, unbounded",
	.insns = {
	BPF_MOV64_IMM(BPF_REG_2, 6),
	BPF_MOV64_IMM(BPF_REG_3, 28),
	/* Fill the top 16 bytes of the stack. */
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -16, 0),
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0),
	/* Get an unknown value. */
	BPF_LDX_MEM(BPF_DW, BPF_REG_4, BPF_REG_1, offsetof(struct bpf_sock_ops,
							   bytes_received)),
	/* Check the lower bound but don't check the upper one. */
	BPF_JMP_IMM(BPF_JSLT, BPF_REG_4, 0, 4),
	/* Point the lower bound to initialized stack. Offset is now in range
	 * from fp-16 to fp+0x7fffffffffffffef, i.e. max value is unbounded.
	 */
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_4, 16),
	BPF_ALU64_REG(BPF_ADD, BPF_REG_4, BPF_REG_10),
	BPF_MOV64_IMM(BPF_REG_5, 8),
	/* Dereference it indirectly. */
	BPF_EMIT_CALL(BPF_FUNC_getsockopt),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.errstr = "R4 unbounded indirect variable offset stack access",
	.result = REJECT,
	.prog_type = BPF_PROG_TYPE_SOCK_OPS,
},
{
	"indirect variable-offset stack access, max out of bound",
	.insns = {
	/* Fill the top 8 bytes of the stack */
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0),
	/* Get an unknown value */
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_2, 4),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_2, 8),
	/* add it to fp.  We now have either fp-4 or fp-8, but
	 * we don't know which
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_10),
	/* dereference it indirectly */
	BPF_LD_MAP_FD(BPF_REG_1, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.fixup_map_hash_8b = { 5 },
	.errstr = "R2 max value is outside of stack bound",
	.result = REJECT,
	.prog_type = BPF_PROG_TYPE_LWT_IN,
},
{
	"indirect variable-offset stack access, min out of bound",
	.insns = {
	/* Fill the top 8 bytes of the stack */
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0),
	/* Get an unknown value */
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_2, 4),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_2, 516),
	/* add it to fp.  We now have either fp-516 or fp-512, but
	 * we don't know which
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_10),
	/* dereference it indirectly */
	BPF_LD_MAP_FD(BPF_REG_1, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.fixup_map_hash_8b = { 5 },
	.errstr = "R2 min value is outside of stack bound",
	.result = REJECT,
	.prog_type = BPF_PROG_TYPE_LWT_IN,
},
{
	"indirect variable-offset stack access, max_off+size > max_initialized",
	.insns = {
	/* Fill only the second from top 8 bytes of the stack. */
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -16, 0),
	/* Get an unknown value. */
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned. */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_2, 4),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_2, 16),
	/* Add it to fp.  We now have either fp-12 or fp-16, but we don't know
	 * which. fp-12 size 8 is partially uninitialized stack.
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_10),
	/* Dereference it indirectly. */
	BPF_LD_MAP_FD(BPF_REG_1, 0),
	BPF_EMIT_CALL(BPF_FUNC_map_lookup_elem),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.fixup_map_hash_8b = { 5 },
	.errstr = "invalid indirect read from stack var_off",
	.result = REJECT,
	.prog_type = BPF_PROG_TYPE_LWT_IN,
},
{
	"indirect variable-offset stack access, min_off < min_initialized",
	.insns = {
	/* Fill only the top 8 bytes of the stack. */
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0),
	/* Get an unknown value */
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned. */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_2, 4),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_2, 16),
	/* Add it to fp.  We now have either fp-12 or fp-16, but we don't know
	 * which. fp-16 size 8 is partially uninitialized stack.
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_10),
	/* Dereference it indirectly. */
	BPF_LD_MAP_FD(BPF_REG_1, 0),
	BPF_EMIT_CALL(BPF_FUNC_map_lookup_elem),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.fixup_map_hash_8b = { 5 },
	.errstr = "invalid indirect read from stack var_off",
	.result = REJECT,
	.prog_type = BPF_PROG_TYPE_LWT_IN,
},
{
	"indirect variable-offset stack access, priv vs unpriv",
	.insns = {
	/* Fill the top 16 bytes of the stack. */
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -16, 0),
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0),
	/* Get an unknown value. */
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned. */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_2, 4),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_2, 16),
	/* Add it to fp.  We now have either fp-12 or fp-16, we don't know
	 * which, but either way it points to initialized stack.
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_10),
	/* Dereference it indirectly. */
	BPF_LD_MAP_FD(BPF_REG_1, 0),
	BPF_EMIT_CALL(BPF_FUNC_map_lookup_elem),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.fixup_map_hash_8b = { 6 },
	.errstr_unpriv = "R2 stack pointer arithmetic goes out of range, prohibited for !root",
	.result_unpriv = REJECT,
	.result = ACCEPT,
	.prog_type = BPF_PROG_TYPE_CGROUP_SKB,
},
{
	"indirect variable-offset stack access, uninitialized",
	.insns = {
	BPF_MOV64_IMM(BPF_REG_2, 6),
	BPF_MOV64_IMM(BPF_REG_3, 28),
	/* Fill the top 16 bytes of the stack. */
	BPF_ST_MEM(BPF_W, BPF_REG_10, -16, 0),
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0),
	/* Get an unknown value. */
	BPF_LDX_MEM(BPF_W, BPF_REG_4, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned. */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_4, 4),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_4, 16),
	/* Add it to fp.  We now have either fp-12 or fp-16, we don't know
	 * which, but either way it points to initialized stack.
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_4, BPF_REG_10),
	BPF_MOV64_IMM(BPF_REG_5, 8),
	/* Dereference it indirectly. */
	BPF_EMIT_CALL(BPF_FUNC_getsockopt),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.errstr = "invalid indirect read from stack var_off",
	.result = REJECT,
	.prog_type = BPF_PROG_TYPE_SOCK_OPS,
},
{
	"indirect variable-offset stack access, ok",
	.insns = {
	/* Fill the top 16 bytes of the stack. */
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -16, 0),
	BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0),
	/* Get an unknown value. */
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	/* Make it small and 4-byte aligned. */
	BPF_ALU64_IMM(BPF_AND, BPF_REG_2, 4),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_2, 16),
	/* Add it to fp.  We now have either fp-12 or fp-16, we don't know
	 * which, but either way it points to initialized stack.
	 */
	BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_10),
	/* Dereference it indirectly. */
	BPF_LD_MAP_FD(BPF_REG_1, 0),
	BPF_EMIT_CALL(BPF_FUNC_map_lookup_elem),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.fixup_map_hash_8b = { 6 },
	.result = ACCEPT,
	.prog_type = BPF_PROG_TYPE_LWT_IN,
},
