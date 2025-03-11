/*
 * Copyright(c) 2012-2021 Intel Corporation
 * Copyright(c) 2024 Huawei Technologies
 * SPDX-License-Identifier: BSD-3-Clause
 */

// #include "spdk/bdev_module.h"
#include "ocf_core_status.h"
#include "module/bdev/ocf/vbdev_ocf.h"


bool vbdev_ocf_io_is_blocked(struct ocf_request *req){
    struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)req->io.priv1;
	if (bdev_io->bdev->internal.status_runtime == SPDK_BDEV_RUNTIME_STATUS_BUSY) {
		return true;
	} else {
		return false;
	}
}

bool vbdev_ocf_core_is_blocked(struct ocf_core *core){
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(ocf_core_get_volume(core)));
	if (base->bdev->internal.status_runtime == SPDK_BDEV_RUNTIME_STATUS_BUSY) {
		return true;
	} else {
		return false;
	}
}