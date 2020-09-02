.. _openAMP_sample:

IPC library Sample Application
##########################

Overview
********

This application demonstrates how to use OpenAMP with Zephyr using
IPC transport library. It is designed to demonstrate how to integrate
OpenAMP with Zephyr both from a build perspective and code.

Library can work in two modes:
 - using automatic shared memory allocation
 - using manual shared memory allocation.

To chose desired mode enable IPC_AUTO_SHMEM_ALLOCATE configuration
in kConfig file.
In the first mode user must define how many instances to use. It is
done in prj.conf file in variable @ref IPC_INSTANCES_NO. 
The library prepares its internal array containing IPC instances
and its count is equal to @ref IPC_INSTANCES_NO. It also slices
shared memory into @ref IPC_INSTANCES_NO. memory slabs.
To start work user must obtain IPC instance, init it and register
at least one endpoint. 

In the second mode user must define in source code its own IPC
instances. Despite that fact, user must provide information in kConfig
file how many instances to use (variable @ref IPC_INSTANCES_NO).
Shared memory configuration for provided instance can be obtained using
proper function. from provided API.
