/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016 - 2020 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "erpc_arbitrated_client_setup.h"
#include "erpc_mbf_setup.h"
#include "erpc_server_setup.h"
#include "erpc_transport_setup.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "test_firstInterface_server.h"
#include "test_secondInterface.h"
#include "unit_test.h"

#ifdef __cplusplus
extern "C" {
#endif
#if defined(RPMSG)
#include "rpmsg_lite.h"
#endif
#include "app_core1.h"
#include "board.h"
#include "mcmgr.h"
#if defined(__CC_ARM) || defined(__ARMCC_VERSION)
int main(int argc, char **argv);
#endif
#ifdef __cplusplus
}
#endif
using namespace erpc;

int testClient();

#define APP_ERPC_READY_EVENT_DATA (1)

SemaphoreHandle_t g_waitQuitMutex;
TaskHandle_t g_serverTask;
TaskHandle_t g_clientTask;

volatile int waitQuit = 0;
volatile int waitClient = 0;
volatile int isTestPassing = 0;
uint32_t startupData;
mcmgr_status_t status;
volatile int stopTest = 0;
extern const uint32_t erpc_generated_crc;
erpc_service_t service = NULL;
erpc_server_t server;

////////////////////////////////////////////////////////////////////////////////
// Code
////////////////////////////////////////////////////////////////////////////////
void increaseWaitQuit()
{
    xSemaphoreTake(g_waitQuitMutex, portMAX_DELAY);
    waitQuit++;
    xSemaphoreGive(g_waitQuitMutex);
}

void runServer(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    erpc_status_t err;
    err = erpc_server_run(server);
    increaseWaitQuit();

    if (err != kErpcStatus_Success)
    {
        // server error
        while (1)
        {
        }
    }
    vTaskSuspend(NULL);
}

void runClient(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // wait until ERPC first (client) app will announce that it is ready.
    while (waitClient == 0)
    {
        vTaskDelay(10);
    }

    // wait until ERPC first (client) app will announce ready to quit state
    while (true)
    {
        isTestPassing = testClient();

        if (waitQuit != 0 || isTestPassing != 0 || stopTest != 0)
        {
            enableFirstSide();
            break;
        }
        vTaskDelay(10);
    }

    while (true)
    {
        if (waitQuit != 0)
        {
            break;
        }
        vTaskDelay(100);
    }

    // send to ERPC first (client) app ready to quit state
    quitSecondInterfaceServer();
    increaseWaitQuit();
    vTaskSuspend(NULL);
}

static void SignalReady(void)
{
    /* Signal the other core we are ready by trigerring the event and passing the APP_ERPC_READY_EVENT_DATA */
    MCMGR_TriggerEvent(kMCMGR_RemoteApplicationEvent, APP_ERPC_READY_EVENT_DATA);
}

/*!
 * @brief Application-specific implementation of the SystemInitHook() weak function.
 */
void SystemInitHook(void)
{
    /* Initialize MCMGR - low level multicore management library. Call this
       function as close to the reset entry as possible to allow CoreUp event
       triggering. The SystemInitHook() weak function overloading is used in this
       application. */
    MCMGR_EarlyInit();
}

void runInit(void *arg)
{
    // Initialize MCMGR before calling its API
    MCMGR_Init();

    // Get the startup data
    do
    {
        status = MCMGR_GetStartupData(&startupData);
    } while (status != kStatus_MCMGR_Success);

    erpc_transport_t transportClient;
    erpc_transport_t transportServer;
    erpc_mbf_t message_buffer_factory;
    erpc_client_t client;

#if defined(RPMSG)
    // RPMsg-Lite transport layer initialization
    transportClient = erpc_transport_rpmsg_lite_rtos_remote_init(101, 100, (void *)startupData, 0, SignalReady, NULL);

    // MessageBufferFactory initialization
    message_buffer_factory = erpc_mbf_rpmsg_init(transportClient);
#elif defined(MU)
    // MU transport layer initialization
    transportClient = erpc_transport_mu_init(MU_BASE);
    message_buffer_factory = erpc_mbf_dynamic_init();
#endif

    // eRPC client side initialization
    client = erpc_arbitrated_client_init(transportClient, message_buffer_factory, &transportServer);

    // eRPC server side initialization
    server = erpc_server_init(transportServer, message_buffer_factory);

    erpc_arbitrated_client_set_crc(client, erpc_generated_crc);

    // adding server to client for nested calls.
    erpc_arbitrated_client_set_server(client, server);
    erpc_arbitrated_client_set_server_thread_id(client, (void *)g_serverTask);

    // adding the service to the server
    service = create_FirstInterface_service();
    erpc_add_service_to_server(server, service);

#if defined(MU)
    SignalReady();
#endif

    // unblock server and client task
    xTaskNotifyGive(g_serverTask);
    xTaskNotifyGive(g_clientTask);

    // Wait until client side will stop.
    while (true)
    {
        if (waitQuit >= 3)
        {
            break;
        }
        vTaskDelay(100);
    }

    vTaskSuspend(NULL);
}

int main(int argc, char **argv)
{
    BOARD_InitHardware();

    g_waitQuitMutex = xSemaphoreCreateMutex();
    xTaskCreate(runInit, "runInit", 256, NULL, 1, NULL);
    xTaskCreate(runServer, "runServer", 1536, NULL, 3, &g_serverTask);
    xTaskCreate(runClient, "runClient", 1536, NULL, 2, &g_clientTask);

    vTaskStartScheduler();

    while (1)
    {
    }
}

void stopSecondSide()
{
    ++stopTest;
}

int32_t getResultFromSecondSide()
{
    return isTestPassing;
}

void testCasesAreDone(void)
{
    increaseWaitQuit();
}

void quitFirstInterfaceServer()
{
    /* removing the service from the server */
    erpc_remove_service_from_server(server, service);
    destroy_FirstInterface_service(service);

    // Stop server part
    erpc_server_stop(server);
}

void whenReady()
{
    waitClient++;
}

int testClient()
{
    int number = 15;
    for (int i = 0; i < number; i++)
    {
        secondSendInt(i + number);
    }
    for (int i = number - 1; i >= 0; i--)
    {
        if (i + number != secondReceiveInt())
        {
            return -1;
        }
    }
    return 0;
}
