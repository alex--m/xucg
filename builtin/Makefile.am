#
# Copyright (c) Huawei Technologies Co., Ltd. 2019.  ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

noinst_LTLIBRARIES         = libucg_builtin.la
libucg_builtin_la_CFLAGS   = $(BASE_CFLAGS)
libucg_builtin_la_CPPFLAGS = $(BASE_CPPFLAGS)

noinst_HEADERS = \
	ops/builtin_ops.h \
	ops/builtin_comp_step.inl \
	plan/builtin_plan.h

libucg_builtin_la_SOURCES = \
	builtin.c \
	ops/builtin_op.c \
	ops/builtin_pack.c \
	ops/builtin_reduce.c \
	ops/builtin_step_create.c \
	ops/builtin_step_execute.c \
	plan/builtin_binomial_tree.c \
	plan/builtin_bruck.c \
	plan/builtin_pairwise.c \
	plan/builtin_recursive.c \
	plan/builtin_tree.c \
	plan/builtin_ring.c \
	plan/builtin_topo_info.c
