/*
 * Copyright(c) 2012-2022 Intel Corporation
 * Copyright(c) 2024 Huawei Technologies
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ocf/ocf.h"
#include "../ocf_priv.h"
#include "../ocf_cache_priv.h"
#include "../ocf_queue_priv.h"
#include "../ocf_seq_cutoff.h"
#include "cache_engine.h"
#include "engine_common.h"
#include "engine_rd.h"
#include "engine_wt.h"
#include "engine_pt.h"
#include "engine_wi.h"
#include "engine_wa.h"
#include "engine_wb.h"
#include "engine_wo.h"
#include "engine_fast.h"
#include "engine_flush.h"
#include "engine_discard.h"
#include "../utils/utils_user_part.h"
#include "../utils/utils_refcnt.h"
#include "../ocf_request.h"
#include "../metadata/metadata.h"
#include "../ocf_space.h"

enum ocf_io_if_type {
	/* Public OCF IO interfaces to be set by user */
	OCF_IO_WT_IF,
	OCF_IO_WB_IF,
	OCF_IO_WA_IF,
	OCF_IO_WI_IF,
	OCF_IO_PT_IF,
	OCF_IO_WO_IF,
	OCF_IO_MAX_IF,

	/* Private OCF interfaces */
	OCF_IO_FAST_IF,
	OCF_IO_FLUSH_IF,
	OCF_IO_DISCARD_IF,
	OCF_IO_PRIV_MAX_IF,
};

static const struct ocf_io_if IO_IFS[OCF_IO_PRIV_MAX_IF] = {
	[OCF_IO_WT_IF] = {
		.cbs = {
			[OCF_READ] = ocf_read_generic,
			[OCF_WRITE] = ocf_write_wt,
		},
		.name = "Write Through"
	},
	[OCF_IO_WB_IF] = {
		.cbs = {
			[OCF_READ] = ocf_read_generic,
			[OCF_WRITE] = ocf_write_wb,
		},
		.name = "Write Back"
	},
	[OCF_IO_WA_IF] = {
		.cbs = {
			[OCF_READ] = ocf_read_generic,
			[OCF_WRITE] = ocf_write_wa,
		},
		.name = "Write Around"
	},
	[OCF_IO_WI_IF] = {
		.cbs = {
			[OCF_READ] = ocf_read_generic,
			[OCF_WRITE] = ocf_write_wi,
		},
		.name = "Write Invalidate"
	},
	[OCF_IO_PT_IF] = {
		.cbs = {
			[OCF_READ] = ocf_read_pt, // read req cutoff过来的，没有cache dirty data就能直接读，不行就还是走read_generic
			[OCF_WRITE] = ocf_write_wi, // write req cutoff过来的，采用write-invalidate模式，将cache里的数据给invalid了然后将数据直接写到base core
		},
		.name = "Pass Through",
	},
	[OCF_IO_WO_IF] = {
		.cbs = {
			[OCF_READ] = ocf_read_wo,
			[OCF_WRITE] = ocf_write_wb,
		},
		.name = "Write Only",
	},
	[OCF_IO_FAST_IF] = {
		.cbs = {
			[OCF_READ] = ocf_read_fast,
			[OCF_WRITE] = ocf_write_fast,
		},
		.name = "Fast",
	},
	[OCF_IO_FLUSH_IF] = {
		.cbs = {
			[OCF_READ] = ocf_engine_flush,
			[OCF_WRITE] = ocf_engine_flush,
		},
		.name = "Flush",
	},
	[OCF_IO_DISCARD_IF] = {
		.cbs = {
			[OCF_READ] = ocf_engine_discard,
			[OCF_WRITE] = ocf_engine_discard,
		},
		.name = "Discard",
	},
};

static const struct ocf_io_if *cache_mode_io_if_map[ocf_req_cache_mode_max] = {
	[ocf_req_cache_mode_wt] = &IO_IFS[OCF_IO_WT_IF],
	[ocf_req_cache_mode_wb] = &IO_IFS[OCF_IO_WB_IF],
	[ocf_req_cache_mode_wa] = &IO_IFS[OCF_IO_WA_IF],
	[ocf_req_cache_mode_wi] = &IO_IFS[OCF_IO_WI_IF],
	[ocf_req_cache_mode_wo] = &IO_IFS[OCF_IO_WO_IF],
	[ocf_req_cache_mode_pt] = &IO_IFS[OCF_IO_PT_IF],
	[ocf_req_cache_mode_fast] = &IO_IFS[OCF_IO_FAST_IF],
};

const char *ocf_get_io_iface_name(ocf_req_cache_mode_t cache_mode)
{
	if (cache_mode == ocf_req_cache_mode_max)
		return "Unknown";

	return cache_mode_io_if_map[cache_mode]->name;
}

static ocf_req_cb ocf_cache_mode_to_engine_cb(
		ocf_req_cache_mode_t req_cache_mode, int rw)
{
	if (req_cache_mode == ocf_req_cache_mode_max)
		return NULL;

	return cache_mode_io_if_map[req_cache_mode]->cbs[rw];
}

bool ocf_fallback_pt_is_on(ocf_cache_t cache)
{
	ENV_BUG_ON(env_atomic_read(&cache->fallback_pt_error_counter) < 0);

	return (cache->fallback_pt_error_threshold !=
			OCF_CACHE_FALLBACK_PT_INACTIVE &&
			env_atomic_read(&cache->fallback_pt_error_counter) >=
			cache->fallback_pt_error_threshold);
}

void ocf_resolve_effective_cache_mode(ocf_cache_t cache,
		ocf_core_t core, struct ocf_request *req)
{
	ocf_cache_mode_t cache_mode;

	if (ocf_fallback_pt_is_on(cache)){ // 非test模式不会经过这个函数
		req->cache_mode = ocf_req_cache_mode_pt;
		return;
	}

	if (cache->pt_unaligned_io && !ocf_req_is_4k(req->addr, req->bytes)) { // Use pass-through mode for I/O requests unaligned to 4KiB，当且仅当req不是4K的倍数时，使用pt模式
		req->cache_mode = ocf_req_cache_mode_pt;
		return;
	}

	if (req->core_line_count > cache->conf_meta->cachelines) { // req自身的core_line_count大于cache的cacheline数量，则使用pt模式
		req->cache_mode = ocf_req_cache_mode_pt;
		return;
	}

	if (ocf_core_seq_cutoff_check(core, req)) { // 判断是否对这个IO使用seq_cutoff，如果满足条件，则使用pt模式，这里使用bdev的status判断
		req->cache_mode = ocf_req_cache_mode_pt;
		req->seq_cutoff = 1;
		return;
	}

	cache_mode = ocf_user_part_get_cache_mode(cache,
			ocf_user_part_class2id(cache, req->part_id));

	if (!ocf_cache_mode_is_valid(cache_mode))
		cache_mode = cache->conf_meta->cache_mode;

	req->cache_mode = ocf_cache_mode_to_req_cache_mode(cache_mode);

	if (req->rw == OCF_WRITE &&
	    ocf_req_cache_mode_has_lazy_write(req->cache_mode) &&
	    ocf_req_set_dirty(req)) { // 这一步将req设置为脏的同时，又使得req的cache_mode变为了pt，因为这时候说明req的dirty字段是freeze的，也就是说需要使用write-through模式（cache和core始终同步）
		req->cache_mode = ocf_req_cache_mode_wt;
	}
}

int ocf_engine_hndl_req(struct ocf_request *req)
{
	ocf_cache_t cache = req->cache;

	OCF_CHECK_NULL(cache);

	req->engine_handler = ocf_cache_mode_to_engine_cb(req->cache_mode,
			req->rw);

	if (!req->engine_handler)
		return -OCF_ERR_INVAL;

	ocf_req_get(req);

	/* Till OCF engine is not synchronous fully need to push OCF request
	 * to into OCF workers
	 */

	ocf_queue_push_req(req, OCF_QUEUE_ALLOW_SYNC);

	return 0;
}

int ocf_engine_hndl_fast_req(struct ocf_request *req) // fast request入口
{
	ocf_req_cb engine_cb;
	int ret;

	engine_cb = ocf_cache_mode_to_engine_cb(req->cache_mode, req->rw);
	if (!engine_cb)
		return -OCF_ERR_INVAL;

	ocf_req_get(req);

	ret = engine_cb(req); // 判定当前的req能不能走fastpath，当且仅当write req且全部map、read req且全部hit没有dirty data才能走fastpath

	if (ret == OCF_FAST_PATH_NO)
		ocf_req_put(req);

	return ret;
}

void ocf_engine_hndl_discard_req(struct ocf_request *req)
{
	ocf_req_get(req);

	IO_IFS[OCF_IO_DISCARD_IF].cbs[req->rw](req);
}

void ocf_engine_hndl_flush_req(struct ocf_request *req)
{
	ocf_req_get(req);

	req->engine_handler = IO_IFS[OCF_IO_FLUSH_IF].cbs[req->rw];

	ocf_queue_push_req(req, OCF_QUEUE_ALLOW_SYNC);
}

bool ocf_req_cache_mode_has_lazy_write(ocf_req_cache_mode_t mode)
{
	return ocf_cache_mode_is_valid((ocf_cache_mode_t)mode) &&
			ocf_mngt_cache_mode_has_lazy_write(
					(ocf_cache_mode_t)mode);
}
