#
# Copyright (c) 2020 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: Apache-2.0
#

zephyr_include_directories_ifdef(CONFIG_OPENAMP_MULTIINSTANCE .)
zephyr_sources_ifdef(CONFIG_OPENAMP_MULTIINSTANCE multi_instance.c)
