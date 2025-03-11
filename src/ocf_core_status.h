/*
 * Copyright(c) 2012-2021 Intel Corporation
 * Copyright(c) 2024 Huawei Technologies
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __OCF_CORE_STATUS_H__
#define __OCF_CORE_STATUS_H__

#include "ocf_request.h"
#include "ocf_core_priv.h"
#include "ocf/ocf_core.h"

bool vbdev_ocf_io_is_blocked(struct ocf_request *req);

bool vbdev_ocf_core_is_blocked(struct ocf_core *core);

#endif /* __OCF_CORE_STATUS_H__ */