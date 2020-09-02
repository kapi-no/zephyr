/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This library enables user to use OpenAmp library to enable communication
 * between master and remote core of the processor.
 * This library supports two mechanisms of shared memory allocation.
 * It is controlled by @ref IPC_AUTO_SHMEM_ALLOCATE kConfig symbol.
 *  - autoallocate: Memory is sliced based on maximum allowable virtqueue
 *                  size (it is suited to available shared memory).
 *                  Library creates array of instances and endpoints.
 *                  Sizes of these arrays depends on
 *                  @ref IPC_INSTANCES_NO and @ref IPC_ENDPOINTS_NO symbols.
 *
 *  - custom allocation: User has possibility to define virtqueue size that
 *                  meets its requirements.
 * When memory allocation mechanism is set, following steps are required:
 *  1. Initialize ipc, by calling @ref ipc_init()
 *  2. Initialize endpoints by calling @ref ipc_ept_init()
 *
 * When those steps are completed, application is ready to exchange data
 * between two cores.
 */
#include <zephyr.h>
#include <errno.h>
#include <drivers/ipm.h>
#include <../rpmsg/rpmsg_internal.h>

#include "multi_instance.h"
#include <openamp/virtio_ring.h>
#include <string.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(multi_inst_ipc, CONFIG_LOG_DEFAULT_LEVEL);

/* Shared memory configuration */

#define VRING_COUNT           ( 2 ) /**< Number of used vring buffers. */

#define EP_FLAG_HANSHAKE_DONE ( 0 ) /**< Indicates that handshake
                                         was done. */
#define EP_FLAG_USED          ( 1 ) /**< Flag used to defined that
                                         IPC instance is used. */

#define IPC_FLAG_USED         ( 1 ) /**< Flag used to defined that
                                         IPC instance is used. */

#define IPC_VQ_0             ( 0 ) /**< TX virtqueue queue index. */
#define IPC_VQ_1             ( 1 ) /**< RX virtqueue queue index. */

#define RPMSG_LOCATE_DATA(p) ((unsigned char *)(p) + sizeof(struct rpmsg_hdr))

#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
static struct
{
    struct ipc_ept_t  epts[CONFIG_IPC_ENDPOINTS_NO]; /* Holds all defined endpoints. */
    struct ipc_inst_t inst[CONFIG_IPC_INSTANCES_NO]; /* Holds all defined IPC instances. */
} ipc_ctx;
#endif /* CONFIG_IPC_AUTO_SHMEM_ALLOCATE */

/** @brief Function for getting virtio status.
 *
 * @param[in] vdev Pointer to virtio device structure.
 *
 * @retval VIRTIO_CONFIG_STATUS_DRIVER_OK if device is MASTER.
 *         Read a byte from a memory mapped register if device is REMOTE.
 */
static unsigned char virtio_get_status(struct virtio_device * p_vdev)
{
    struct ipc_inst_t * p_ipc = metal_container_of(p_vdev,
                                                   struct ipc_inst_t,
                                                   vdev);

    return (IS_ENABLED(CONFIG_RPMSG_MASTER) ?
            VIRTIO_CONFIG_STATUS_DRIVER_OK :
            sys_read8(p_ipc->shmem_status_reg_addr));
}

/** @brief Function for getting virtio features.
 *
 * @param[in] vdev Pointer to virtio device structure.
 *
 * @retval Virio features.
 */
static uint32_t virtio_get_features(struct virtio_device * vdev)
{
    return BIT(VIRTIO_RPMSG_F_NS);
}

#ifdef CONFIG_RPMSG_MASTER
/** @brief Function for setting virtio status.
 *
 * @param[in] vdev   Pointer to virtio device structure.
 * @param[in] status Status to set.
 *
 * @retval None.
 */
static void virtio_set_status(struct virtio_device * p_vdev, unsigned char status)
{
    struct ipc_inst_t * p_ipc = metal_container_of(p_vdev,
                                                   struct ipc_inst_t,
                                                   vdev);
    sys_write8(status, p_ipc->shmem_status_reg_addr);
}

/** @brief Function for setting virtio features.
 *
 * @param[in] vdev     Pointer to virtio device structure.
 * @param[in] features Features to set.
 *
 * @retval None.
 */
static void virtio_set_features(struct virtio_device *vdev, uint32_t features)
{
    /* No need for implementation */
}
#endif

/** @brief Function for sending the virtio notification.
 *         This function sends notification to
 *         assigned receiver using IPM.
 *
 * @param[in] vq Pointer to the virtqueue structure.
 *
 * @retval None.
 */
static void virtio_notify(struct virtqueue * vq)
{
    int status;

    struct ipc_inst_t * p_ipc = metal_container_of(vq->vq_dev,
                                                   struct ipc_inst_t,
                                                   vdev);
    if (p_ipc)
    {
        status = ipm_send(p_ipc->ipm_tx_handle, 0, 0, NULL, 0);
        if (status != 0)
        {
            LOG_WRN("Failed to notify: %d", status);
        }
    }

}

/* @brief Collection of virtio related functions.
 * */
const struct virtio_dispatch dispatch =
{
    .get_status = virtio_get_status,
    .get_features = virtio_get_features,
#ifdef CONFIG_RPMSG_MASTER
    .set_status = virtio_set_status,
    .set_features = virtio_set_features,
#endif
    .notify = virtio_notify,
};

/** @brief Function for configuration shared memory for provided instance.
 *
 * @note Shared memory is divided into smaller regions:
 *
 *       Region name | STATUS | VIRTQUEUE BUFFERS | VRING_RX | VRING_TX |
 *
 *       Sizes:
 *          RPMSG_BUFFER_SIZE : 512 bytes
 *          VRING_SIZE        : Defined by user.
 *          STATUS            : 1 byte.
 *          VIRTQUEUE BUFFERS : VIRTQUEUE MASTER-RX / REMOTE-TX |
 *                              VIRTQUEUE MASTER-TX / REMOTE-RX |
 *          VRING_TX          : Obtained by func VRING_SIZE_COMPUTE()
 *          VRING_RX          : Obtained by func VRING_SIZE_COMPUTE()
 *
 *          VIRTQUEUE MASTER-RX / REMOTE-TX : RPMSG_BUFFER_SIZE * VRING_SIZE
 *          VIRTQUEUE MASTER-TX / REMOTE-RX : RPMSG_BUFFER_SIZE * VRING_SIZE
 *
 * @param[in] p_ipm    Pointer to the IPC instance.
 * @param[in] p_config Pointer to the configuration structure.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
static int ipc_configure_shmem(struct ipc_inst_t * p_ipm,
                               struct ipc_config_t const * p_config)
{
    p_ipm->shmem_status_reg_addr = p_config->shmem_addr;

    uint32_t shmem_local_start_addr = p_config->shmem_addr + VDEV_STATUS_SIZE;
    uint32_t shmem_local_size = p_config->shmem_size - (VDEV_STATUS_SIZE);

    p_ipm->shm_physmap[0] = shmem_local_start_addr;

    p_ipm->shm_device.name = "sram0.shm";
    p_ipm->shm_device.bus = NULL;
    p_ipm->shm_device.num_regions = 1;

    p_ipm->shm_device.regions->virt = (void*) shmem_local_start_addr;
    p_ipm->shm_device.regions->physmap = p_ipm->shm_physmap;

    p_ipm->shm_device.regions->size = shmem_local_size;
    p_ipm->shm_device.regions->page_shift = 0xffffffff;
    p_ipm->shm_device.regions->page_mask = 0xffffffff;
    p_ipm->shm_device.regions->mem_flags = 0;

    p_ipm->shm_device.irq_num = 0;
    p_ipm->shm_device.irq_info = NULL;

    uint32_t rpmsg_reg_size = 2 * VIRTQUEUE_SIZE_GET(p_config->vring_size);
    uint32_t vring_region_size = VRING_SIZE_COMPUTE(p_config->vring_size, VRING_ALIGNMENT);

    p_ipm->vring_rx_address = shmem_local_start_addr + rpmsg_reg_size;
    p_ipm->vring_tx_address = p_ipm->vring_rx_address + vring_region_size;

    return 0;
}

/** @brief Function for destroying desired endpoint.
 *
 * @param[in] p_ep Pointer to the endpoint structure.
 *
 * @retval None.
 */
static void rpmsg_service_unbind(struct rpmsg_endpoint * p_ep)
{
    rpmsg_destroy_ept(p_ep);
}

/** @brief Function for getting virtio RX buffer.
 *
 * Retrieves the received buffer from the virtqueue.
 *
 * @param p_rvdev Pointer to rpmsg device.
 * @param len     Size of received buffer.
 * @param idx     Index of buffer.
 *
 * @return Pointer to received buffer.
 */
static void* rpmsg_virtio_get_rx_buffer(struct rpmsg_virtio_device * p_rvdev,
                                        size_t                     * len,
                                        uint16_t                   * idx)
{
    return (IS_ENABLED(CONFIG_RPMSG_MASTER) ?
                       virtqueue_get_buffer(p_rvdev->rvq, len, idx) :
                       virtqueue_get_available_buffer(p_rvdev->rvq, idx, len));
}

/** @brief Function for returning virtio RX buffer.
 *
 *  This function releases virtio rx buffer.
 *
 * @param p_rvdev  Pointer to rpmsg virtio device.
 * @param p_buffer Pointer to the buffer.
 * @param len      Buffer length.
 * @param idx      Buffer index.
 *
 * @return Virtqueue status.
 */
static int rpmsg_virtio_return_buffer(struct rpmsg_virtio_device * p_rvdev,
                                       void                      * p_buffer,
                                       size_t                      len,
                                       uint16_t                    idx)
{
    int status = 0;
    if (IS_ENABLED(CONFIG_RPMSG_MASTER))
    {
        struct virtqueue_buf vqbuf;

        (void) idx;
        /* Initialize buffer node */
        vqbuf.buf = p_buffer;
        vqbuf.len = len;
        status = virtqueue_add_buffer(p_rvdev->rvq, &vqbuf, 0, 1, p_buffer);
    }
    else
    {
        (void) p_buffer;
        status = virtqueue_add_consumed_buffer(p_rvdev->rvq, idx, len);
    }
    return status;
}

/** @brief Function for notifying user that message has arrived.
 *
 *  Callback is called right after some data has arrived.
 *  This callback is executed in interrupt context. Therefore, use only
 *  interrupt-safe APIs.
 *
 * @param context    Arbitrary context pointer provided at registration time.
 * @param id Pointer Message type identifier.
 * @param data       Message data pointer.
 *
 * @return None.
 */
static void ipm_callback(struct device *ipmdev, void * context,
                         uint32_t id, volatile void * data)
{
    struct ipc_inst_t * p_ipm = (struct ipc_inst_t*) context;

    __ASSERT(p_ipm->irq_cb != NULL, "Null pointer as irq_cb");
    p_ipm->irq_cb(p_ipm->arg);
}

/** @brief Function for copying received message to users buffer.
 *
 *  For now this function uses blocking memcpy, but there is possibility
 *  to add non-blocking mechanism.
 *  This function notifies user when copying is done via callback
 *  binded to the endpoint.
 *
 * @param p_ept    Pointer to the endpoint.
 * @param p_source Pointer to the source buffer.
 * @param p_dest   Pointer to the buffer where the data will be copied.
 * @param size     Size of the data to copy.
 *
 * @return None.
 */
static void ipc_copy_data(struct ipc_ept_t * p_ept,
                          void         * p_source,
                          void         * p_dest,
                          size_t         size)
{

#if IS_ENABLED(CONFIG_IPC_COPY_NO_BLOCKING)
    BUILD_ASSERT(!IS_ENABLED(CONFIG_IPC_COPY_NO_BLOCKING), "Not implemented yet.");
#endif
    /* Copy data. */
    memcpy(p_dest, p_source, size);

    /* Data has been copied. Notify user about that. */
    if (p_ept->cb)
    {
        p_ept->cb(IPC_EVENT_DATA, p_dest, size, p_ept->arg);
    }
}

int ipc_init(struct ipc_inst_t         * p_ipm,
             struct ipc_config_t const * p_config,
             irq_handler                 irq_cb,
             void                      * arg)
{
    if (!irq_cb)
    {
        return -EINVAL;
    }

    int err;

    p_ipm->irq_cb = irq_cb;
    p_ipm->arg = arg;
    p_ipm->metal_params = (struct metal_init_params) METAL_INIT_DEFAULTS;

    err = ipc_configure_shmem(p_ipm, p_config);
    if (err)
    {
        LOG_ERR("shmem configuration: failed - error code %d", err);
        return err;
    }

    /* Libmetal setup. */
    err = metal_init(&p_ipm->metal_params);
    if (err)
    {
        LOG_ERR("metal_init: failed - error code %d", err);
        return err;
    }

    err = metal_register_generic_device(&p_ipm->shm_device);
    if (err)
    {
        LOG_ERR("Couldn't register shared memory device: %d", err);
        return err;
    }

    err = metal_device_open("generic", "sram0.shm", &p_ipm->device);
    if (err)
    {
        LOG_ERR("metal_device_open failed: %d", err);
        return err;
    }

    p_ipm->shm_io = metal_device_io_region(p_ipm->device, 0);
    if (!p_ipm->shm_io)
    {
        LOG_ERR("metal_device_io_region failed to get region");
        return -ENODEV;
    }

    /* IPM setup. */
    p_ipm->ipm_tx_handle = device_get_binding(p_config->ipm_name_tx);
    if (!p_ipm->ipm_tx_handle)
    {
        LOG_ERR("Could not get TX IPM device handle");
        return -ENODEV;
    }

    p_ipm->ipm_rx_handle = device_get_binding(p_config->ipm_name_rx);
    if (!p_ipm->ipm_rx_handle)
    {
        LOG_ERR("Could not get RX IPM device handle");
        return -ENODEV;
    }

    /* Register IPM callback. This cb executes when msg has come. */
    ipm_register_callback(p_ipm->ipm_rx_handle, ipm_callback, p_ipm);

    /* Virtqueue setup. */
    p_ipm->vq[IPC_VQ_0] = virtqueue_allocate(p_config->vring_size);
    if (!p_ipm->vq[IPC_VQ_0])
    {
        LOG_ERR("virtqueue_allocate failed to alloc vq[IPC_VQ_0]");
        return -ENOMEM;
    }

    p_ipm->vq[IPC_VQ_1] = virtqueue_allocate(p_config->vring_size);
    if (!p_ipm->vq[IPC_VQ_1])
    {
        LOG_ERR("virtqueue_allocate failed to alloc vq[IPC_VQ_1]");
        return -ENOMEM;
    }

    p_ipm->rvrings[IPC_VQ_0].io = p_ipm->shm_io;
    p_ipm->rvrings[IPC_VQ_0].info.vaddr = (void*) p_ipm->vring_tx_address;
    p_ipm->rvrings[IPC_VQ_0].info.num_descs = p_config->vring_size;
    p_ipm->rvrings[IPC_VQ_0].info.align = VRING_ALIGNMENT;
    p_ipm->rvrings[IPC_VQ_0].vq = p_ipm->vq[IPC_VQ_0];

    p_ipm->rvrings[IPC_VQ_1].io = p_ipm->shm_io;
    p_ipm->rvrings[IPC_VQ_1].info.vaddr = (void*) p_ipm->vring_rx_address;
    p_ipm->rvrings[IPC_VQ_1].info.num_descs = p_config->vring_size;
    p_ipm->rvrings[IPC_VQ_1].info.align = VRING_ALIGNMENT;
    p_ipm->rvrings[IPC_VQ_1].vq = p_ipm->vq[IPC_VQ_1];

    p_ipm->vdev.role = IS_ENABLED(CONFIG_RPMSG_MASTER) ?
                                  RPMSG_MASTER : RPMSG_REMOTE;

    p_ipm->vdev.vrings_num = VRING_COUNT;
    p_ipm->vdev.func = &dispatch;
    p_ipm->vdev.vrings_info = &p_ipm->rvrings[0];

    if (IS_ENABLED(CONFIG_RPMSG_MASTER))
    {
        /* This step is only required if you are VirtIO device master.
         * Initialize the shared buffers pool. */
        rpmsg_virtio_init_shm_pool(&p_ipm->shpool,
                                   (void*) p_ipm->shm_device.regions->virt,
                                    p_ipm->shm_device.regions->size);

    }
    err = rpmsg_init_vdev(&p_ipm->rvdev,
                          &p_ipm->vdev,
                          NULL,
                          p_ipm->shm_io,
                          &p_ipm->shpool);
    if (err)
    {
        LOG_ERR("RPMSG vdev initialization failed %d", err);
        return err;
    }

    /* Get RPMsg device from RPMsg VirtIO device. */
    p_ipm->rdev = rpmsg_virtio_get_rpmsg_device(&p_ipm->rvdev);

#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    /* Mark this instance as used. */
    WRITE_BIT(p_ipm->flags, IPC_FLAG_USED, 1);
#endif /* CONFIG_IPC_AUTO_SHMEM_ALLOCATE */

    return 0;
}

void ipc_deinit(struct ipc_inst_t * p_ipm)
{
    rpmsg_deinit_vdev(&p_ipm->rvdev);
    metal_finish();
    LOG_INF("IPC deinitialised");
}

#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE

struct ipc_ept_t * ipc_endpoint_get_next_free(void)
{
    for (uint8_t i = 0; i < CONFIG_IPC_ENDPOINTS_NO; i++)
    {
        if(!(ipc_ctx.epts[i].flags & BIT(EP_FLAG_USED)))
        {
            return &ipc_ctx.epts[i];
        }
    }
    /* No more endoints avaiable. */
    return NULL;
}

struct ipc_inst_t * ipc_inst_get_next_free(void)
{
    for (uint8_t i = 0; i < CONFIG_IPC_INSTANCES_NO; i++)
    {
        if(!(ipc_ctx.inst[i].flags & BIT(IPC_FLAG_USED)))
        {
            return &ipc_ctx.inst[i];
        }
    }
    /* No more endoints avaiable. */
    return NULL;
}

#endif /* CONFIG_IPC_AUTO_SHMEM_ALLOCATE */

int ipc_ept_init(struct ipc_inst_t             * p_ipm,
                 struct ipc_ept_t              * p_ept,
                 struct ipc_ept_config_t const * p_config)
{
    if (!p_config->cb)
    {
        return -EINVAL;
    }

    int err = 0;
    p_ept->cb = p_config->cb;
    p_ept->arg = p_config->arg;

    err = rpmsg_create_ept(&p_ept->ep,
                            p_ipm->rdev,
                            "",
                            p_config->ept_no,
                            p_config->ept_no,
                            NULL,
                            rpmsg_service_unbind);

    if (err != 0)
    {
        LOG_ERR("RPMSG endpoint create failed %d", err);
        return err;
    }
#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    /* Mark endpoints as busy. */
    WRITE_BIT(p_ept->flags, EP_FLAG_USED, 1);
#endif /* CONFIG_IPC_AUTO_SHMEM_ALLOCATE */

    /* Notify binded endpoint. It is a part
     * of the handshake mechanism. */
    return rpmsg_send(&p_ept->ep, (uint8_t*) "", 0);
}

int ipc_send(struct ipc_ept_t * p_ept, const void * p_buff, size_t size)
{
    if(!p_ept)
    {
        return -EINVAL;
    }

    /* Check if endpoint is binded. */
    if (!(p_ept->flags & BIT(EP_FLAG_HANSHAKE_DONE)))
    {
        return -ENOTCONN;
    }

    int ret = rpmsg_send(&p_ept->ep, (uint8_t*) p_buff, size);

    if (ret == size)
    {
        return 0;
    }
    else
    {
        if (ret < 0)
        {
            return ret;
        }
        else
        {
            return -EMSGSIZE;
        }
    }
}

int ipc_free(struct ipc_inst_t * p_ipm)
{
    struct virtio_device * vdev = (IS_ENABLED(CONFIG_RPMSG_MASTER) ?
                                              p_ipm->vq[0]->vq_dev :
                                              p_ipm->vq[1]->vq_dev);
    struct rpmsg_virtio_device * rvdev = vdev->priv;
    struct rpmsg_device * rdev = &rvdev->rdev;

    /* User has been notified. Now we can free occupied memory region. */
    metal_mutex_acquire(&rdev->lock);

    /* Return used buffers. */
    int status = rpmsg_virtio_return_buffer(rvdev,
                                        p_ipm->rp_hdr,
                                        p_ipm->msg_len,
                                        p_ipm->msg_idx);
    if(status)
    {
        LOG_WRN("Failed to free the buffer: %d", status);
        metal_mutex_release(&rdev->lock);
        return -ECANCELED;
    }

    virtqueue_kick(rvdev->rvq);
    metal_mutex_release(&rdev->lock);

    return 0;
}

int ipc_recv(struct ipc_inst_t * p_ipm, void * payload)
{
    struct virtio_device * vdev = (IS_ENABLED(CONFIG_RPMSG_MASTER) ?
                                              p_ipm->vq[0]->vq_dev :
                                              p_ipm->vq[1]->vq_dev);
    struct rpmsg_virtio_device * rvdev = vdev->priv;
    struct rpmsg_device * rdev = &rvdev->rdev;
    struct rpmsg_endpoint * ept;

    bool continue_reading = false;
    do
    {
        metal_mutex_acquire(&rdev->lock);

        /* Process the received data from remote node */
        p_ipm->rp_hdr = rpmsg_virtio_get_rx_buffer(rvdev,
                                                  &p_ipm->msg_len,
                                                  &p_ipm->msg_idx);
        if(p_ipm->rp_hdr)
        {
            /* Get the channel node from the remote device channels list. */
            ept = rpmsg_get_endpoint(rdev,
                                     NULL,
                                     p_ipm->rp_hdr->dst,
                                     RPMSG_ADDR_ANY);
            metal_mutex_release(&rdev->lock);

            if (ept)
            {
                if (ept->dest_addr == RPMSG_ADDR_ANY)
                {
                    /*
                     * First message received from the remote side,
                     * update channel destination address
                     */
                    ept->dest_addr = p_ipm->rp_hdr->src;
                }
                /* Copy data from the shared memory to the buffer. */
                struct ipc_ept_t * my_ep = metal_container_of(ept, struct ipc_ept_t, ep);

                if (p_ipm->rp_hdr->len == 0)
                {
                    /* Handle endpoint handshake*/
                    if (!(my_ep->flags & BIT(EP_FLAG_HANSHAKE_DONE)))
                    {
                        int status = rpmsg_send(ept, (uint8_t*) "", 0);
                        if (status != 0)
                        {
                            return status;
                        }
                        WRITE_BIT(my_ep->flags, EP_FLAG_HANSHAKE_DONE, 1);
                        LOG_INF("Handshake done");
                        my_ep->cb(IPC_EVENT_CONNECTED, NULL, 0, my_ep->arg);
                    }
                    else
                    {
                        /* We got empty message and there is no point to inform user about that.
                         * Just ignore message and free the buffer.*/
                        ipc_free(p_ipm);
                    }
                    continue_reading = true;
                }
                else
                {
                    ipc_copy_data(my_ep,
                                  RPMSG_LOCATE_DATA(p_ipm->rp_hdr),
                                  payload,
                                  p_ipm->rp_hdr->len);
                    continue_reading = false;
                }
            }
            else
            {
                /* We received data but address of destination endpoint
                 * is unknown. Ignore message. */
                ipc_free(p_ipm);
                return -ENXIO;
            }
        }
        else
        {
            /* There is no message to handle. */
            metal_mutex_release(&rdev->lock);
            LOG_INF("EMPTY");
            return -ENOENT;
        }


    } while(continue_reading);

    return 0;
}
