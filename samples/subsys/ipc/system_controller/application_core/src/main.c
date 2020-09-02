/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

/** @file
 *  @brief Nordic IPC multi instance library example.
 */
#include <zephyr/types.h>
#include <zephyr.h>
#include <logging/log.h>

#include <device.h>
#include <soc.h>

#include <stdio.h>
#include <string.h>
#include <init.h>

#include "multi_instance.h"

LOG_MODULE_REGISTER(app, CONFIG_LOG_DEFAULT_LEVEL);

/* Thread related defines. */
#define STACKSIZE         ( 1024 )/* Size of stack area used by each thread */
#define PRIORITY          ( 7 )   /* Scheduling priority used by each thread */

#define BUFF_SIZE         ( 16 )  /* Size of the buffer. */

#define SHM_NODE            DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR      DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE            DT_REG_SIZE(SHM_NODE)

#define INST_1_VIRTQ_SIZE ( 8 )  /* Size of the virtqueue of first instance.
                                    Used in non-autoallocation mode. */
#define INST_2_VIRTQ_SIZE ( 16 ) /* Size of the virtqueue of second instance.
                                    Used in non-autoallocation mode. */

/* @brief IPC event handler prototype. */
typedef void (*irq_handler)(void * arg);

/* @brief Helper structure for collecting all important
 *        data.*/
struct ipc_thread_t
{
    struct ipc_inst_t         * p_ipc;
    struct ipc_config_t const * p_ipc_config;
    struct ipc_ept_t          * p_ept;
    struct ipc_ept_config_t   * p_ept_config;
    struct k_sem              * sem;
    bool                      * binded;
    uint8_t                     inst_idx;
    irq_handler                 irq_cb;
};

K_SEM_DEFINE(rx_sem_t1, 0, 1); /**< Semaphore used for TX sync. */
K_SEM_DEFINE(rx_sem_t2, 0, 1); /**< Semaphore used for TX sync. */

/* Flags */
bool ep_binded_1 = false; /**< Binded indication of first instance. */
bool ep_binded_2 = false; /**< Binded indication of second instance. */

/**@brief Function for handling notification from IPC.
 *
 *  This function notifes that message has come.
 *
 * @param[in] arg Users parameter.
 * @retval None.
 */
static void msg_notification_cb_1(void * arg)
{
    k_sem_give(&rx_sem_t1);
}

/**@brief Function for handling notification from IPC.
 *
 *  This function notifes that message has come.
 *
 * @param[in] arg Users parameter.
 * @retval None.
 */
static void msg_notification_cb_2(void * arg)
{
    k_sem_give(&rx_sem_t2);
}

/**@brief Function for handling IPC messages.
 *
 *  This function is called as callback right after
 *  message is copied from IPC virtqueue to users buffer.
 *
 * @param[in] evt    Event type. Detailed descriotion @ref ipc_event_t.
 * @param[in] p_data Pointer to the users buffer.
 * @param[in] len    Length of the copied data.
 * @param[in] arg    Users argument. It is registered in @ref ipc_ept_config_t.
 * @retval None.
 */
static void ep_handler_1(ipc_event_t   evt,
                         const void  * p_data,
                         size_t        len,
                         void        * arg)
{
    uint8_t * p_buf = (uint8_t*) p_data;
    switch (evt)
    {
        case IPC_EVENT_CONNECTED:
            ep_binded_1 = true;
            break;

        case IPC_EVENT_DATA:
            LOG_HEXDUMP_INF(p_buf, len, "EP1: ");
            break;

        default:
            break;
    }
}

/**@brief Function for handling IPC messages.
 *
 *  This function is called as callback right after
 *  message is copied from IPC virtqueue to users buffer.
 *
 * @param[in] evt    Event type. Detailed descriotion @ref ipc_event_t.
 * @param[in] p_data Pointer to the users buffer.
 * @param[in] len    Length of the copied data.
 * @param[in] arg    Users argument. It is registered in @ref ipc_ept_config_t.
 * @retval None.
 */
static void ep_handler_2(ipc_event_t   evt,
                         const void  * p_data,
                         size_t        len,
                         void        * arg)
{
    uint8_t * p_buf = (uint8_t*) p_data;
    switch (evt)
    {
        case IPC_EVENT_CONNECTED:
            ep_binded_2 = true;
            break;

        case IPC_EVENT_DATA:
            LOG_HEXDUMP_INF(p_buf, len, "EP2: ");
            break;

        default:
            break;
    }
}

/* Variables handler */
#ifndef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
struct ipc_inst_t ipc_1 = { 0 };
struct ipc_inst_t ipc_2 = { 0 };

struct ipc_ept_t ep_1 = { 0 };
struct ipc_ept_t ep_2 = { 0 };
#endif

const struct ipc_config_t ipc_conf_1 =
{
    .ipm_name_tx = "IPM_1",
    .ipm_name_rx = "IPM_0",
#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    .vring_size = VRING_SIZE_GET(SHM_SIZE),
    .shmem_addr = SHMEM_INST_ADDR_AUTOALLOC_GET(SHM_START_ADDR, SHM_SIZE, 0),
    .shmem_size = SHMEM_INST_SIZE_AUTOALLOC_GET(SHM_SIZE),
#else
    .vring_size = INST_1_VIRTQ_SIZE,
    .shmem_addr = SHMEM_INST_ADDR_GET(SHM_START_ADDR, INST_1_VIRTQ_SIZE, 0),
    .shmem_size = SHMEM_INST_SIZE_GET(INST_1_VIRTQ_SIZE),
#endif /* CONFIG_IPC_AUTO_SHMEM_ALLOCATE */
};

const struct ipc_config_t ipc_conf_2 =
{
    .ipm_name_tx = "IPM_3",
    .ipm_name_rx = "IPM_2",
#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    .vring_size = VRING_SIZE_GET(SHM_SIZE),
    .shmem_addr = SHMEM_INST_ADDR_AUTOALLOC_GET(SHM_START_ADDR, SHM_SIZE, 1),
    .shmem_size = SHMEM_INST_SIZE_AUTOALLOC_GET(SHM_SIZE),
#else
    .vring_size = INST_2_VIRTQ_SIZE,
    .shmem_addr = ipc_conf_1.shmem_addr + ipc_conf_1.shmem_size,
    .shmem_size = SHMEM_INST_SIZE_GET(INST_2_VIRTQ_SIZE),
#endif /* CONFIG_IPC_AUTO_SHMEM_ALLOCATE */
};

struct ipc_ept_config_t ept_conf_1 =
{
    .ept_no = 0,
    .cb = ep_handler_1,
#ifndef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    .arg = &ep_1
#endif
};

struct ipc_ept_config_t ept_conf_2 =
{
    .ept_no = 0,
    .cb = ep_handler_2,
#ifndef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    .arg = &ep_2
#endif
};

struct ipc_thread_t ipc_inst_1 =
{
#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    .p_ipc = NULL,
    .p_ept = NULL,
#else
    .p_ipc = &ipc_1,
    .p_ept = &ep_1,
#endif
    .p_ipc_config = &ipc_conf_1,
    .p_ept_config = &ept_conf_1,
    .sem = &rx_sem_t1,
    .binded = &ep_binded_1,
    .inst_idx = 0,
    .irq_cb = msg_notification_cb_1,
};

struct ipc_thread_t ipc_inst_2 =
{
#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    .p_ipc = NULL,
    .p_ept = NULL,
#else
    .p_ipc = &ipc_2,
    .p_ept = &ep_2,
#endif
    .p_ipc_config = &ipc_conf_2,
    .p_ept_config = &ept_conf_2,
    .sem = &rx_sem_t2,
    .binded = &ep_binded_2,
    .inst_idx = 1,
    .irq_cb = msg_notification_cb_2,
};

/**@brief Function for initializing IPC communication.
 *
 * This function initializes IPC instance and sets up one endpoint per instance.
 *
 * @param[in] p_thread_conf Pointer to the thread configuration structure.
 *
 * @retval 0          Send was successful.
 *         -ENOEXEC   Failed to init
 */
int init_ipc (struct ipc_thread_t * p_thread_conf)
{
#ifdef CONFIG_IPC_AUTO_SHMEM_ALLOCATE
    p_thread_conf->p_ipc = ipc_inst_get_next_free();
    if (p_thread_conf->p_ipc == NULL)
    {
        LOG_WRN("IPC: No more free ipc instances\n");
        return -ENOEXEC;
    }

    if (ipc_init(p_thread_conf->p_ipc,
                 p_thread_conf->p_ipc_config,
                 p_thread_conf->irq_cb,
                 NULL) != 0)
    {
        LOG_WRN("IPC: Error during init.");
        return -ENOEXEC;
    }

    p_thread_conf->p_ept = ipc_endpoint_get_next_free();
    if (p_thread_conf->p_ept == NULL)
    {
        LOG_WRN("ep: No more free endpoints\n");
        return -ENOEXEC;
    }

    if (ipc_ept_init(p_thread_conf->p_ipc,
                     p_thread_conf->p_ept,
                     p_thread_conf->p_ept_config) != 0)
    {
        LOG_WRN("EP_1: Error during init.");
        return -ENOEXEC;
    }
#else
    /* Initialize IPC instances. */
    if (ipc_init(p_thread_conf->p_ipc,
                 p_thread_conf->p_ipc_config,
                 p_thread_conf->irq_cb,
                 NULL) != 0)
    {
        LOG_WRN("IPC: Error during init.");
        return -ENOEXEC;
    }

    /* Initialize Endpoints. */
    if (ipc_ept_init(p_thread_conf->p_ipc,
                     p_thread_conf->p_ept,
                     p_thread_conf->p_ept_config) != 0)
    {
        LOG_WRN("EP: Error during init.");
        return -ENOEXEC;
    }
#endif

    return 0;
}

void ipc_thread(void * arg1, void * arg2, void * arg3)
{
    LOG_INF("APP core");

    /* Delay added intentionally to emphasise that library is able
     * to handle different startup times and can initialize despite that fact. */
    k_sleep(K_MSEC(500));

    struct ipc_thread_t * p_thread_conf = (struct ipc_thread_t*) arg1;

    uint8_t payload[BUFF_SIZE] = { 0 };
    uint8_t sent_no = 0; /* Variable for counting sent messages. */
    uint8_t buf[8] = { 0 };
    int status = 0;

    /* Initialize IPC. */
    if (init_ipc(p_thread_conf) != 0)
    {
        LOG_INF("Failed to initialize IPC\n");
    }

    while (1)
    {

        k_sem_take(p_thread_conf->sem, K_FOREVER);

        ipc_recv(p_thread_conf->p_ipc, payload);
        ipc_free(p_thread_conf->p_ipc);

        if (*p_thread_conf->binded)
        {
            k_sleep(K_MSEC(1));

            LOG_INF("Sending %d\n", sent_no);
            sprintf(buf, "MSG: %d\n", sent_no);
            status = ipc_send(p_thread_conf->p_ept, buf, sizeof(buf));
            if ( status != 0)
            {
                LOG_WRN("Sent: failed to send %d, status: %d.", sent_no, status);
            }
            else
            {
                sent_no++;
            }

        }
        k_yield();
    }
}

K_THREAD_DEFINE(tid_1,
                STACKSIZE,
                ipc_thread,
                &ipc_inst_1,
                NULL,
                NULL,
                PRIORITY,
                0,
                0);

K_THREAD_DEFINE(tid_2,
                STACKSIZE,
                ipc_thread,
                &ipc_inst_2,
                NULL,
                NULL,
                PRIORITY,
                0,
                0);
