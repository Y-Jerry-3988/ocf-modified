/*
 * Copyright(c) 2012-2022 Intel Corporation
 * Copyright(c) 2023-2024 Huawei Technologies
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ocf/ocf.h"
#include "../engine/cache_engine.h"
#include "../engine/engine_common.h"
#include "../ocf_request.h"
#include "utils_parallelize.h"

#define OCF_PARALLELIZE_ALIGNMENT 64

struct ocf_parallelize {
	ocf_cache_t cache;
	ocf_parallelize_handle_t handle;
	ocf_parallelize_finish_t finish;
	void *priv;

	unsigned shards_cnt;
	env_atomic remaining;
	env_atomic error;

	struct ocf_request *reqs[];
};

static void _ocf_parallelize_finish(ocf_parallelize_t parallelize)
{
	ocf_parallelize_finish_t finish;
	void *priv;
	int error;

	if (env_atomic_dec_return(&parallelize->remaining))
		return;

	finish = parallelize->finish;
	priv = parallelize->priv;
	error = env_atomic_read(&parallelize->error);

	finish(parallelize, priv, error);
}

static int _ocf_parallelize_hndl(struct ocf_request *req)
{
	ocf_parallelize_t parallelize = req->priv;
	int error;

	error = parallelize->handle(parallelize, parallelize->priv,
			req->byte_position, parallelize->shards_cnt);

	env_atomic_cmpxchg(&parallelize->error, 0, error);

	_ocf_parallelize_finish(parallelize);

	return 0;
}

int ocf_parallelize_create(ocf_parallelize_t *parallelize,
		ocf_cache_t cache, unsigned shards_cnt, uint32_t priv_size,
		ocf_parallelize_handle_t handle,
		ocf_parallelize_finish_t finish)
{
	ocf_parallelize_t tmp_parallelize;
	struct list_head *iter;
	ocf_queue_t queue;
	size_t prl_size;
	unsigned queue_count = 0;
	int result, i;

	queue_count = ocf_cache_get_queue_count(cache);

	if (shards_cnt == 0)
		shards_cnt = queue_count ?: 1;

	prl_size = sizeof(*tmp_parallelize) +
			shards_cnt * sizeof(*tmp_parallelize->reqs);

	tmp_parallelize = env_vzalloc(prl_size + priv_size + OCF_PARALLELIZE_ALIGNMENT);
	if (!tmp_parallelize)
		return -OCF_ERR_NO_MEM;

	if (priv_size > 0) {
		uintptr_t priv = (uintptr_t)tmp_parallelize + prl_size;
		priv = OCF_DIV_ROUND_UP(priv, OCF_PARALLELIZE_ALIGNMENT) * OCF_PARALLELIZE_ALIGNMENT;
		tmp_parallelize->priv = (void*)priv;
	}

	tmp_parallelize->cache = cache;
	tmp_parallelize->handle = handle;
	tmp_parallelize->finish = finish;

	tmp_parallelize->shards_cnt = shards_cnt;
	env_atomic_set(&tmp_parallelize->remaining, shards_cnt + 1);
	env_atomic_set(&tmp_parallelize->error, 0);


	iter = cache->io_queues.next;
	for (i = 0; i < shards_cnt; i++) {
		if (queue_count > 0) {
			queue = list_entry(iter, struct ocf_queue, list);
			iter = iter->next;
			if (iter == &cache->io_queues)
				iter = iter->next;
		} else {
			queue = cache->mngt_queue;
		}
		tmp_parallelize->reqs[i] = ocf_req_new_mngt(cache, queue);
		if (!tmp_parallelize->reqs[i]) {
			result = -OCF_ERR_NO_MEM;
			goto err_reqs;
		}
		tmp_parallelize->reqs[i]->info.internal = true;
		tmp_parallelize->reqs[i]->engine_handler =
			_ocf_parallelize_hndl;
		tmp_parallelize->reqs[i]->byte_position = i;
		tmp_parallelize->reqs[i]->priv = tmp_parallelize;
	}

	*parallelize = tmp_parallelize;

	return 0;

err_reqs:
	while (i--)
		ocf_req_put(tmp_parallelize->reqs[i]);
	env_vfree(tmp_parallelize);

	return result;
}

void ocf_parallelize_destroy(ocf_parallelize_t parallelize)
{
	int i;

	for (i = 0; i < parallelize->shards_cnt; i++)
		ocf_req_put(parallelize->reqs[i]);

	env_vfree(parallelize);
}

void *ocf_parallelize_get_priv(ocf_parallelize_t parallelize)
{
	return parallelize->priv;
}

void ocf_parallelize_set_priv(ocf_parallelize_t parallelize, void *priv)
{
	parallelize->priv = priv;
}

void ocf_parallelize_run(ocf_parallelize_t parallelize)
{
	int i;

	for (i = 0; i < parallelize->shards_cnt; i++)
		ocf_queue_push_req(parallelize->reqs[i], OCF_QUEUE_PRIO_HIGH);

	_ocf_parallelize_finish(parallelize);
}
