/*
 * Copyright(c) 2012-2022 Intel Corporation
 * Copyright(c) 2023-2024 Huawei Technologies
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "engine_pt.h"
#include "engine_rd.h"
#include "engine_common.h"
#include "engine_io.h"
#include "cache_engine.h"
#include "../ocf_request.h"
#include "../utils/utils_user_part.h"
#include "../metadata/metadata.h"
#include "../concurrency/ocf_concurrency.h"

#define OCF_ENGINE_DEBUG_IO_NAME "pt"
#include "engine_debug.h"

static void _ocf_read_pt_complete(struct ocf_request *req, int error)
{
	OCF_DEBUG_RQ(req, "Completion");

	if (error)
		ocf_core_stats_core_error_update(req->core, OCF_READ);

	/* Complete request */
	req->complete(req, error);

	ocf_req_unlock(ocf_cache_line_concurrency(req->cache), req);

	/* Release OCF request */
	ocf_req_put(req);
}

static inline void _ocf_read_pt_submit(struct ocf_request *req)
{
	OCF_DEBUG_RQ(req, "Submit");

	/* Core read */
	ocf_engine_forward_core_io_req(req, _ocf_read_pt_complete);
}

int ocf_read_pt_do(struct ocf_request *req)
{
	/* Get OCF request - increase reference counter */
	ocf_req_get(req);

	if (req->info.dirty_any) {
		ocf_hb_req_prot_lock_rd(req);
		/* Need to clean, start it */
		ocf_engine_clean(req); // 如果有dirty data要首先flush到base device
		ocf_hb_req_prot_unlock_rd(req);

		/* Do not processing, because first we need to clean request */
		ocf_req_put(req);

		return 0;
	}

	if (ocf_engine_needs_repart(req)) {
		OCF_DEBUG_RQ(req, "Re-Part");

		ocf_hb_req_prot_lock_wr(req);

		/* Probably some cache lines are assigned into wrong
		 * partition. Need to move it to new one
		 */
		ocf_user_part_move(req);

		ocf_hb_req_prot_unlock_wr(req);
	}

	/* Submit read IO to the core */
	_ocf_read_pt_submit(req); // 从此入口进入的read req都是保证在base device上有最新数据的，因此直接在base设备上读

	/* Update statistics */
	ocf_engine_update_block_stats(req);

	ocf_core_stats_pt_block_update(req->core, req->part_id, req->rw,
			req->bytes);

	ocf_core_stats_request_pt_update(req->core, req->part_id, req->rw,
			req->info.hit_no, req->core_line_count);

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return 0;
}

int ocf_read_pt(struct ocf_request *req)
{
	bool use_cache = false;
	int lock = OCF_LOCK_NOT_ACQUIRED;

	OCF_DEBUG_TRACE(req->cache);


	/* Get OCF request - increase reference counter */
	ocf_req_get(req);

	/* Set resume handler */
	req->engine_handler = ocf_read_pt_do;

	ocf_req_hash(req);
	ocf_hb_req_prot_lock_rd(req);

	/* Traverse request to check if there are mapped cache lines */
	ocf_engine_traverse(req);

	if (req->seq_cutoff && ocf_engine_is_dirty_all(req) &&
			!req->force_pt) { // 这里可以排除seq_cutoff情况下允许read req直接走base device但又全是dirty line的情况，显然应该走cache直接读
		use_cache = true;
	} else {
		if (ocf_engine_mapped_count(req)) { // 这里是为了read_generic中没有promotion或者因为空间不够导致promote没有成功的read req
			/* There are mapped cache line,
			 * lock request for READ access
			 */
			lock = ocf_req_async_lock_rd(
					req->cache->device->concurrency.
							cache_line,
					req, ocf_engine_on_resume);
		} else {
			/* No mapped cache lines, no need to get lock */
			lock = OCF_LOCK_ACQUIRED; // 这里是read数据全在base的情况下，直接走base device
		}
	}

	ocf_hb_req_prot_unlock_rd(req);

	if (use_cache) {
		/*
		 * There is dirty HIT, and sequential cut off,
		 * because of this force read data from cache
		 */
		ocf_req_clear(req);
		ocf_read_generic(req); //因为有dirty data, seq cutoff和bypass走不通，还是只能走cache
	} else {
		if (lock >= 0) {
			if (lock == OCF_LOCK_ACQUIRED) {
				/* Lock acquired perform read off operations */
				ocf_read_pt_do(req); // 从此入口进入的read req可能在cache上还有dirty数据，因此直接在base设备上读之前要先flush
			} else {
				/* WR lock was not acquired, need to wait for resume */
				OCF_DEBUG_RQ(req, "NO LOCK");
			}
		} else {
			OCF_DEBUG_RQ(req, "LOCK ERROR %d", lock);
			req->complete(req, lock);
			ocf_req_put(req);
		}
	}

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return 0;
}

void ocf_queue_push_req_pt(struct ocf_request *req)
{
	ocf_queue_push_req_cb(req, ocf_read_pt_do,
			OCF_QUEUE_ALLOW_SYNC | OCF_QUEUE_PRIO_HIGH);
}

