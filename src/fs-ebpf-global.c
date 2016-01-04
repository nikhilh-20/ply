#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dtl.h"
#include "provider.h"

enum extract_op {
	EXTRACT_OP_NONE,
	EXTRACT_OP_MASK,
	EXTRACT_OP_SHIFT,
};

static int int32_void_func(enum bpf_func_id func, enum extract_op op,
			   struct ebpf *e, struct fs_node *n)
{
	/* struct reg *dst; */

	emit(e, CALL(func));
	switch (op) {
	case EXTRACT_OP_MASK:
		/* TODO fix kernel to cast imm to u32 on bitwise operators */
		emit(e, ALU_IMM(FS_AND, BPF_REG_0, 0x7fffffff));
		break;
	case EXTRACT_OP_SHIFT:
		emit(e, ALU_IMM(FS_RSH, BPF_REG_0, 32));
		break;
	default:
		break;
	}

	n->dyn->loc.type = FS_LOC_REG;
	n->dyn->loc.reg = 0;
	/* dst = ebpf_reg_get(e); */
	/* if (!dst) */
	/* 	RET_ON_ERR(-EBUSY, "no free regs\n"); */

	/* ebpf_emit(e, MOV(dst->reg, 0)); */
	/* ebpf_reg_bind(e, dst, n);	 */
	return 0;
}

static int gid_compile(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	return int32_void_func(BPF_FUNC_get_current_uid_gid,
			       EXTRACT_OP_SHIFT, e, n);
}

static int uid_compile(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	return int32_void_func(BPF_FUNC_get_current_uid_gid,
			       EXTRACT_OP_MASK, e, n);
}

static int tgid_compile(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	return int32_void_func(BPF_FUNC_get_current_pid_tgid,
			       EXTRACT_OP_SHIFT, e, n);
}

static int pid_compile(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	return int32_void_func(BPF_FUNC_get_current_pid_tgid,
			       EXTRACT_OP_MASK, e, n);
}

static int ns_compile(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	return int32_void_func(BPF_FUNC_ktime_get_ns,
			       EXTRACT_OP_NONE, e, n);
}

static int int_noargs_annotate(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	if (n->call.vargs)
		return -EINVAL;

	n->dyn->type = FS_INT;
	n->dyn->size = 8;
	return 0;
}

/* static int void_noargs_annotate(struct provider *p, struct ebpf *e, struct fs_node *n) */
/* { */
/* 	if (n->call.vargs) */
/* 		return -EINVAL; */

/* 	return 0; */
/* } */

static int comm_compile(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	emit(e, MOV(BPF_REG_1, BPF_REG_10));
	emit(e, ALU_IMM(FS_ADD, BPF_REG_1, n->dyn->loc.addr));
	emit(e, MOV_IMM(BPF_REG_2, n->dyn->ssize));
	emit(e, CALL(BPF_FUNC_get_current_comm));
	n->dyn->loc.type = FS_LOC_STACK;
	return 0;
}

static int comm_annotate(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	if (n->call.vargs)
		return -EINVAL;

	n->dyn->type = FS_STR;
	n->dyn->ssize = 16;
	return 0;
}

static int generic_load_arg(struct ebpf *e, struct fs_node *arg, int *reg)
{
	switch (arg->dyn->type) {
	case FS_INT:
		switch (arg->dyn->loc.type) {
		case FS_LOC_REG:
			if (arg->dyn->loc.reg != *reg)
				emit(e, MOV(*reg, arg->dyn->loc.reg));
			return 0;
		case FS_LOC_STACK:
			emit(e, LDXDW(*reg, arg->dyn->loc.addr, BPF_REG_10));
			return 0;

		default:
			return -EINVAL;

		}
	case FS_STR:
		switch (arg->dyn->loc.type) {
		case FS_LOC_STACK:
			emit(e, MOV(*reg, BPF_REG_10));
			emit(e, ALU_IMM(FS_ADD, *reg, arg->dyn->loc.addr));

			(*reg)++;
			if (*reg > BPF_REG_5)
				return -ENOMEM;

			if (arg->type == FS_STR)
				emit(e, MOV_IMM(*reg, strlen(arg->string) + 1));
			else
				emit(e, MOV_IMM(*reg, arg->dyn->ssize));

			return 0;

		default:
			return -EINVAL;
		}

	default:
		return -ENOSYS;
	}
}

static int trace_compile(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	struct fs_node *varg;
	int err, reg = BPF_REG_0;

	fs_foreach(varg, n->call.vargs) {
		reg++;
		if (reg > BPF_REG_5)
			return -ENOMEM;

		err = generic_load_arg(e, varg, &reg);
		if (err)
			return err;
	}

	emit(e, CALL(BPF_FUNC_trace_printk));
	return 0;
}

static int trace_annotate(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	if (!n->call.vargs)
		return -EINVAL;

	if (n->call.vargs->type != FS_STR)
		return -EINVAL;

	return 0;
}

static struct builtin global_builtins[] = {
	{
		.name = "gid",
		.annotate = int_noargs_annotate,
		.compile  = gid_compile,
	},
	{
		.name = "uid",
		.annotate = int_noargs_annotate,
		.compile  = uid_compile,
	},
	{
		.name = "tgid",
		.annotate = int_noargs_annotate,
		.compile  = tgid_compile,
	},
	{
		.name = "pid",
		.annotate = int_noargs_annotate,
		.compile  = pid_compile,
	},
	{
		.name = "ns",
		.annotate = int_noargs_annotate,
		.compile  = ns_compile,
	},
	{
		.name = "comm",
		.annotate = comm_annotate,
		.compile  = comm_compile,
	},
	{
		.name = "execname",
		.annotate = comm_annotate,
		.compile  = comm_compile,
	},
	{
		.name = "trace",
		.annotate = trace_annotate,
		.compile  = trace_compile,
	},

	{
		.name = "count",
		.annotate = int_noargs_annotate,
		.compile  = NULL,
	},

	{ .name = NULL }
};

int global_compile(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	struct builtin *bi;
	
	for (bi = global_builtins; bi->name; bi++)
		if (!strcmp(bi->name, n->string))
			return bi->compile(p, e, n);

	_e("'%s' unknown", n->string);
	return -ENOENT;	
}

int global_annotate(struct provider *p, struct ebpf *e, struct fs_node *n)
{
	struct builtin *bi;

	for (bi = global_builtins; bi->name; bi++)
		if (!strcmp(bi->name, n->string))
			return bi->annotate(p, e, n);

	_e("'%s' unknown", n->string);
	return -ENOENT;
}
