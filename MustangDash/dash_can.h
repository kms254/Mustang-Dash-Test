/*
 * dash_can.h - FDCAN bring-up to loopback level (STM32 migration plan U7).
 *
 * Scope is deliberately hardware-proof only: init both peripherals in
 * classic CAN 2.0 mode at 500 kbps and provide the `cantest` round-trip
 * (transmit on bus 1, receive on bus 2 with the two buses wire-jumpered)
 * that proves silicon, transceivers, connectors, and termination in one
 * command. CAN -> DashState decode is explicitly a follow-on plan.
 *
 * STM32-only (raw STM32duino HAL, enabled by -D HAL_FDCAN_MODULE_ENABLED
 * in the h743 env); the Teensy build compiles the stubs at the bottom.
 *
 * NOT host-tested: everything here touches HAL registers. Bench-only
 * verification per the plan (U8): `cantest` acks ok across jumpered buses
 * and reports a timeout -- never hangs -- on a silent bus.
 *
 * Bit timing: 500 kbps assuming an 80 MHz FDCAN kernel clock -- prescaler
 * 10, 16 tq/bit (1 + TSEG1 13 + TSEG2 2), sample point ~87.5%. The kernel
 * clock depends on the RCC tree the core sets up, so U8 bench bring-up
 * must verify the measured bit rate and correct the prescaler if the
 * kernel clock differs. Pins (mule placeholders until the U2 schematic):
 * FDCAN1 RX/TX = PB8/PB9 (AF9), FDCAN2 RX/TX = PB5/PB6 (AF9). PA11/12
 * are avoided (USB CDC), PD0/1 are avoided (telltale placeholders).
 */

#ifndef DASH_CAN_H
#define DASH_CAN_H

#if defined(ARDUINO_ARCH_STM32) && defined(HAL_FDCAN_MODULE_ENABLED)

static FDCAN_HandleTypeDef g_can1;
static FDCAN_HandleTypeDef g_can2;
static bool g_can_ok = false;

/* GPIO + clock bring-up for both FDCAN instances (HAL callback, overrides
 * the core's weak definition) */
extern "C" void HAL_FDCAN_MspInit(FDCAN_HandleTypeDef *hfdcan)
{
    (void)hfdcan;
    __HAL_RCC_FDCAN_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF9_FDCAN1;
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9; /* FDCAN1 RX/TX */
    HAL_GPIO_Init(GPIOB, &g);
    g.Alternate = GPIO_AF9_FDCAN2;
    g.Pin = GPIO_PIN_5 | GPIO_PIN_6; /* FDCAN2 RX/TX */
    HAL_GPIO_Init(GPIOB, &g);
}

static bool dash_can_init_one(FDCAN_HandleTypeDef *h, FDCAN_GlobalTypeDef *inst)
{
    h->Instance = inst;
    h->Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    h->Init.Mode = FDCAN_MODE_NORMAL;
    h->Init.AutoRetransmission = ENABLE;
    h->Init.TransmitPause = DISABLE;
    h->Init.ProtocolException = DISABLE;
    /* 500 kbps @ 80 MHz kernel clock: 80/10 = 8 MHz tq, 16 tq/bit */
    h->Init.NominalPrescaler = 10U;
    h->Init.NominalSyncJumpWidth = 1U;
    h->Init.NominalTimeSeg1 = 13U;
    h->Init.NominalTimeSeg2 = 2U;
    /* data phase unused in classic mode, but the fields must be sane */
    h->Init.DataPrescaler = 10U;
    h->Init.DataSyncJumpWidth = 1U;
    h->Init.DataTimeSeg1 = 13U;
    h->Init.DataTimeSeg2 = 2U;
    /* H7 message RAM: give each instance a minimal, non-overlapping slice */
    h->Init.MessageRAMOffset = (inst == FDCAN1) ? 0U : 512U;
    h->Init.StdFiltersNbr = 1U;
    h->Init.ExtFiltersNbr = 0U;
    h->Init.RxFifo0ElmtsNbr = 8U;
    h->Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
    h->Init.RxFifo1ElmtsNbr = 0U;
    h->Init.RxBuffersNbr = 0U;
    h->Init.TxEventsNbr = 0U;
    h->Init.TxBuffersNbr = 0U;
    h->Init.TxFifoQueueElmtsNbr = 4U;
    h->Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    h->Init.TxElmtSize = FDCAN_DATA_BYTES_8;

    if (HAL_OK != HAL_FDCAN_Init(h))
    {
        return false;
    }

    /* accept-all into FIFO0 */
    FDCAN_FilterTypeDef f = {0};
    f.IdType = FDCAN_STANDARD_ID;
    f.FilterIndex = 0U;
    f.FilterType = FDCAN_FILTER_RANGE;
    f.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    f.FilterID1 = 0x000U;
    f.FilterID2 = 0x7FFU;
    (void)HAL_FDCAN_ConfigFilter(h, &f);
    (void)HAL_FDCAN_ConfigGlobalFilter(h, FDCAN_ACCEPT_IN_RX_FIFO0,
                                       FDCAN_ACCEPT_IN_RX_FIFO0,
                                       FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);

    return (HAL_OK == HAL_FDCAN_Start(h));
}

/* Bring up both buses; logged, never fatal (the dash must not depend on
 * CAN, mirroring the flash-probe discipline). */
static void dash_can_init(void)
{
    /* review fix: init both unconditionally and report per-bus -- a
     * short-circuit && hid which bus failed and skipped the second init */
    const bool ok1 = dash_can_init_one(&g_can1, FDCAN1);
    const bool ok2 = dash_can_init_one(&g_can2, FDCAN2);
    g_can_ok = ok1 && ok2;
    Serial.printf("FDCAN init: ECU=%s PMU=%s (classic 500 kbps)\r\n",
                  ok1 ? "ok" : "FAILED", ok2 ? "ok" : "FAILED");
}

/* `cantest`: send one frame on bus 1, expect it on bus 2 (buses jumpered
 * CANH-CANH / CANL-CANL on the bench). Bounded 100 ms poll -- a silent bus
 * reports a timeout, never hangs. Returns true on round-trip success. */
static bool dash_can_test(void)
{
    if (!g_can_ok)
    {
        return false;
    }

    /* review fix: drain any stale frames first, and stamp the payload with
     * a per-invocation nonce -- a frame left queued by a prior silent-bus
     * run (AutoRetransmission) must not satisfy a later test */
    while (HAL_FDCAN_GetRxFifoFillLevel(&g_can2, FDCAN_RX_FIFO0) > 0U)
    {
        FDCAN_RxHeaderTypeDef drop;
        uint8_t drop_data[8];
        (void)HAL_FDCAN_GetRxMessage(&g_can2, FDCAN_RX_FIFO0, &drop, drop_data);
    }

    FDCAN_TxHeaderTypeDef tx = {0};
    tx.Identifier = 0x123U;
    tx.IdType = FDCAN_STANDARD_ID;
    tx.TxFrameType = FDCAN_DATA_FRAME;
    tx.DataLength = FDCAN_DLC_BYTES_8;
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch = FDCAN_BRS_OFF;
    tx.FDFormat = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    const uint32_t nonce = millis();
    uint8_t payload[8] = { 'M', 'D', 'S', 'H',
                           (uint8_t)(nonce >> 24), (uint8_t)(nonce >> 16),
                           (uint8_t)(nonce >> 8), (uint8_t)nonce };

    if (HAL_OK != HAL_FDCAN_AddMessageToTxFifoQ(&g_can1, &tx, payload))
    {
        return false;
    }

    const uint32_t t0 = millis();
    while ((millis() - t0) < 100UL)
    {
        if (HAL_FDCAN_GetRxFifoFillLevel(&g_can2, FDCAN_RX_FIFO0) > 0U)
        {
            FDCAN_RxHeaderTypeDef rx;
            uint8_t data[8] = {0};
            if (HAL_OK == HAL_FDCAN_GetRxMessage(&g_can2, FDCAN_RX_FIFO0, &rx, data))
            {
                return (0x123U == rx.Identifier)
                       && (0 == memcmp(payload, data, 8U));
            }
        }
    }
    return false; /* timeout: transceiver, jumper, or termination problem */
}

#else /* Teensy / non-FDCAN build: CAN hardware lives on the STM32 carrier */

static void dash_can_init(void)
{
}

static bool dash_can_test(void)
{
    return false; /* no FDCAN on this target */
}

#endif

#endif /* DASH_CAN_H */
