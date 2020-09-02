/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OPENAMP_MULTI_INSTANCE_H
#define OPENAMP_MULTI_INSTANCE_H

/**
 * @file
 * @defgroup rpmsg.h IPC multi instance communication library
 * @{
 * @brief IPC multi instance communication library.
 *        This library enables user to use multiple OpenAmp
 *        instances and multiple endpoints for each instance.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <openamp/open_amp.h>

#include <metal/sys.h>
#include <metal/device.h>
#include <metal/alloc.h>

#define IPC_NO_OF_VQ       ( 2 )   /* Number of used virtqueues. */

#define IPC_INSTANCE_COUNT ( CONFIG_IPC_INSTANCES_NO ) /**< Total number of
                                                            IPC instances.*/
#define VRING_ALIGNMENT    ( 4 )   /**< Alignment of vring buffer.*/

#define VDEV_STATUS_SIZE   ( 0x4 ) /**< Size of status region. */

/* Private macros. */
#define VRING_DESC_SIZEOF(num) ((num) * (sizeof(struct vring_desc)))
#define VRING_AVAIL_SIZEOF(num) (sizeof(struct vring_avail) +             \
                                ((num) * sizeof(uint16_t)) +              \
                                sizeof(uint16_t))
#define VRING_ALIGN(size, align) (((size) + (align) - 1) & ~((align) - 1))
#define VRING_USED_SIZEOF(num) (sizeof(struct vring_used) +               \
                               ((num) * sizeof(struct vring_used_elem)) + \
                               sizeof(uint16_t))

#define VRING_FIRST_SUM(num) (VRING_DESC_SIZEOF(num) + VRING_AVAIL_SIZEOF(num))

/* Public macros */
/* Compute size of vring buffer basen on its size and allignment. */
#define VRING_SIZE_COMPUTE(vring_size, align) (VRING_ALIGN(VRING_FIRST_SUM((vring_size)),(align)) + \
                                               VRING_USED_SIZEOF((vring_size)))

/* Macro for calculating used memory by virtqueue buffers for remote device. */
#define VIRTQUEUE_SIZE_GET(vring_size)  (RPMSG_BUFFER_SIZE * (vring_size))

/* Macro for getting the size of shared memory occupied by single IPC instance. */
#define SHMEM_INST_SIZE_GET(vring_size) (VDEV_STATUS_SIZE +                      \
                                        (2 * VIRTQUEUE_SIZE_GET((vring_size))) + \
                                        (2 * VRING_SIZE_COMPUTE((vring_size),(VRING_ALIGNMENT))))

/* Returns size of used shared memory consumed by all IPC instances*/
#define SHMEM_CONSUMED_SIZE_GET(vring_size) ( IPC_INSTANCE_COUNT * SHMEM_INST_SIZE_GET((vring_size)))

/* Returns maximum allowable size of vring buffers to fit memory requirements. */
#define VRING_SIZE_GET(shmem_size) ((SHMEM_CONSUMED_SIZE_GET(32)) < (shmem_size) ? 32 :\
                                    (SHMEM_CONSUMED_SIZE_GET(16)) < (shmem_size) ? 16 :\
                                    (SHMEM_CONSUMED_SIZE_GET(8))  < (shmem_size) ? 8  :\
                                    (SHMEM_CONSUMED_SIZE_GET(4))  < (shmem_size) ? 4  :\
                                    (SHMEM_CONSUMED_SIZE_GET(2))  < (shmem_size) ? 2 : 1)

/* Returns size of used shared memory of single instance in case of using
 * maximum allowable vring buffer size. */
#define SHMEM_INST_SIZE_AUTOALLOC_GET(shmem_size) (SHMEM_INST_SIZE_GET(VRING_SIZE_GET((shmem_size))))

/* Returns start address of ipc instance in shared memory. It assumes that
 * maximum allowable vring buffer size is used. */
#define SHMEM_INST_ADDR_AUTOALLOC_GET(shmem_addr, shmem_size, id) ((shmem_addr) + ((id) * (SHMEM_INST_SIZE_AUTOALLOC_GET(shmem_size))))

/* Returns start address of ipc instance in shared memory.
 * It calculates offset based on vring_size.
 * */
#define SHMEM_INST_ADDR_GET(shmem_addr, vring_size, id) ((shmem_addr) + ((id) * (SHMEM_INST_SIZE_GET(vring_size))))

/** @brief Asynchronous event notification type. */
typedef enum
{
    IPC_EVENT_CONNECTED, /**< @brief Endpoint was created on the other side and handshake
                                     was successful. */
    IPC_EVENT_ERROR,     /**< @brief Endpoint was not able to connect. */
    IPC_EVENT_DATA,      /**< @brief New packet arrived. */
} ipc_event_t;

/** @brief IRQ handler prototype. */
typedef void (*irq_handler)(void * arg);

/** @brief Endpoint event handler prototype. */
typedef void (*ept_handler)(ipc_event_t  evt,
                            void const * p_data,
                            size_t       len,
                            void       * arg);

/** @brief Endpoint structure. */
struct ipc_ept_t
{
    struct rpmsg_endpoint ep;    /**< Endpoint. */
    ept_handler           cb;    /**< Callback to endpoint handler. */
    void                * arg;   /**< Pointer to user argument. */
    uint32_t              flags; /**< Endpoint flags. */
};

/** @brief Endpoint configuration structure. */
struct ipc_ept_config_t
{
    int         ept_no; /**< Number of the endpoint. It must be unique value. */
    ept_handler cb;     /**< Endpoint callback. It executes when
                             copying data to provided buffer finishes. */
    void      * arg;    /**< Users argument. It is passed through the callback. */
};

/** @brief IPC configuration structure. */
struct ipc_config_t
{
    const char * ipm_name_tx; /* Name of the TX IPM channel. */
    const char * ipm_name_rx; /* Name of the RX IPM channel. */
    uint8_t      vring_size;  /* Number of used slots in virtqueue. */
    uint32_t     shmem_addr;  /* Address of the assigned shared memory region for
                                 that particular instance. */
    uint32_t     shmem_size;  /* Size of the assigned shared memory for that instance. */
};

/** @brief IPC structure. */
struct ipc_inst_t
{
    void (*irq_cb)(void * arg); /**< IRQ Callback. It notifies the users
                                     application that msg has arrived. */
    void * arg;                 /* Custom parameter. User can use it. */

    struct device              * ipm_tx_handle; /* TX handler */
    struct device              * ipm_rx_handle; /* RX handler */

    uint32_t                     shmem_status_reg_addr;
    struct rpmsg_virtio_device   rvdev;
    struct rpmsg_virtio_shm_pool shpool;
    struct rpmsg_device        * rdev;
    struct virtqueue           * vq[IPC_NO_OF_VQ];

    struct virtio_vring_info     rvrings[IPC_NO_OF_VQ];
    struct virtio_device         vdev;

    uint32_t                     vring_tx_address;
    uint32_t                     vring_rx_address;

    struct metal_init_params     metal_params;
    struct metal_io_region     * shm_io;
    struct metal_device        * device;

    metal_phys_addr_t            shm_physmap[1];
    struct metal_device          shm_device;

    /* Elements used for handling message. */
    struct rpmsg_hdr           * rp_hdr;
    uint32_t                     msg_len;
    uint16_t                     msg_idx;
    uint32_t                     flags;
};

/** @brief Function for initializing IPC instance.
 *
 * This function initializes IPM instance. It is important
 * to set desired target in the rpmsg.c file (REMOTE or MASTER).
 *
 * @param[in] p_ipm    Pointer to the IPC instance.
 * @param[in] p_config Pointer to the configuration structure.
 * @param[in] irq_cb   IRQ callback. Used to notify users application that
 *                     message has arrived. To fetch it, user must call
 *                     @ref ipc_recv. This callback executes in interrupt context.
 *                     Therefore, use only interrupt-safe API.
 * @param[in] arg      Pointer to the users argument. It is passed in @ref irq_cb.
 *
 * @retval 0 If the operation was successful.
 *         -ENODEV Failed to get TX or RX IPM handle.
 *         -ENOMEM Not enough memory to register virtqueue.
 *         RPMSG_ERROR_BASE related errors in case of rpmsg faults.
 */
int ipc_init(struct ipc_inst_t         * p_ipm,
             struct ipc_config_t const * p_config,
             irq_handler                 irq_cb,
             void                      * arg);

/**@brief Function for deinitializing IPC instance.
 *
 * This function deinitialize IPC instance.
 */
void ipc_deinit(struct ipc_inst_t * p_ipm);

/**@brief Function for sending data to desired endpoint.
 *
 * @param[in] p_ept  Pointer to the endpoint.
 * @param[in] p_buff Pointer to the buffer to send.
 * @param[in] size   Size of the buffer to send.
 *
 * @retval 0         Send was successful.
 *         -EINVAL   Provided endpoint is incorrect.
 *         -ENOTCONN Unable to send message to the endpoint without handshake.
 *         -EMSGSIZE Failed to send desired number of bytes.
 *         RPMSG_ERROR_BASE related errors in case of rpmsg faults.
 */
int ipc_send(struct ipc_ept_t * p_ept, const void * p_buff, size_t size);

/**@brief Function for receiving msg from queue.
 *
 * This functions starts process of getting message from
 * the queue. It copies message to the provided buffer
 * and calls endpoint callback.
 * @param[in] p_ipm     Pointer to the IPC instance.
 * @param[in] p_payload Pointer to the buffer for the incoming messages.
 * @retval 0      Operation was successful.
 *         -ENXIO  Data addressed to unknown endpoint.
 *         -ENOENT There is no available message to get.
 *         RPMSG_ERROR_BASE related errors in case of rpmsg faults.
 */
int ipc_recv(struct ipc_inst_t * p_ipm, void * payload);

/**@brief Function for freeing msg from internal queue.
 *
 * @param[in] p_ipm Pointer to the IPC instance.
 * @retval 0 If the operation was successful.
 *         -ECANCELED Failed to free the buffer.
 */
int ipc_free(struct ipc_inst_t * p_ipm);

/**@brief Function for initializing endpoint for specified IPC instance.
 *
 * Each endpoint must have corresponding one on the other side (remote/master).
 * This library uses simple handshake mechanism to ensure that endpoints
 * are binded.
 * When handshake is successful, user receives callback.
 *
 * @param[in] p_ipm    Pointer to the IPC instance.
 * @param[in] p_ipm    Pointer to the endpoint instance.
 * @param[in] p_config Pointer to the endpoint configuration structure.
 *
 * @retval 0 If the operation was successful.
 *         RPMSG_ERROR_BASE related errors in case of rpmsg faults.
 */
int ipc_ept_init(struct ipc_inst_t             * p_ipm,
                 struct ipc_ept_t              * p_ept,
                 struct ipc_ept_config_t const * p_config);

/**@brief Function for getting endpoint unique address.
 *
 * @param[in] p_ipm Pointer to the endpoint instance.
 *
 * @retval Endpoint address ff the operation was successful.
 *         -EINVAL Invalid input parameter.
 */
static inline int ipc_ept_num_get(struct ipc_ept_t const * p_ept)
{
    if (!p_ept)
    {
        return -EINVAL;
    }
    return p_ept->ep.addr;
}

#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
/**@brief Function getting pointer to next free endpoint.
 *
 * @param[in] None.
 * @retval Pointer to the endpoint if it is avaiable.
 *         NULL if no more endpoints are avaiable.
 */
struct ipc_ept_t * ipc_endpoint_get_next_free(void);

/**@brief Function getting pointer to next free IPC instance.
 *
 * @param[in] None.
 * @retval Pointer to the IPC instance if it is avaiable.
 *         NULL if no more IPC instances are avaiable.
 */
struct ipc_inst_t * ipc_inst_get_next_free(void);

#endif /* CONFIG_IPC_AUTO_SHMEM_ALLOCATE */

#ifdef __cplusplus
}
#endif

/**
 *@}
 */

#endif /* OPENAMP_MULTI_INSTANCE_H  */
