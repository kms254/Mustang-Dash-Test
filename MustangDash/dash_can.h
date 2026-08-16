/*
 * dash_can.h - FDCAN hardware layer: bring-up, cantest, and the RX drain.
 *
 * Scope is the HAL boundary: init both peripherals in classic CAN 2.0 mode
 * at 500 kbps, provide the `cantest` round-trip (transmit on bus 1, receive
 * on bus 2 with the two buses wire-jumpered) that proves silicon,
 * transceivers, connectors, and termination in one command, and drain
 * FDCAN1's FIFO into the Ford dialect decoder. CAN -> DashState decode
 * itself lives in dash_can_ford.h (pure, host-tested; plan 2026-08-15-001)
 * -- this file only moves bytes and counts frames.
 *
 * STM32-only (raw STM32duino HAL, enabled by -D HAL_FDCAN_MODULE_ENABLED
 * in the h743 env); builds without FDCAN compile the stubs at the bottom.
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

/* ---- dialect seam (review restructure, 2026-08-15) ----
 * This header knows no dialect: the RX drain walks a table of registered
 * decoders and the first one to consume a frame ends the walk. Dialects
 * must not share message IDs (KTD5's one-dialect-per-channel invariant
 * starts at the ID level), so walk order never decides anything. Adding
 * RaceCapture or PMU16 is one dash_can_register_dialect() call in the
 * sketch -- no edit here. Signatures mirror dash_can_ford.h's caller
 * contract; `state` is the dialect's own struct, threaded back as void*. */
#include "dash_data.h"

typedef bool (*DashCanDecodeFn)(void *state, uint32_t id, uint8_t dlc,
                                const uint8_t *bytes, uint32_t now_ms,
                                DashState *s);
typedef void (*DashCanExpireFn)(void *state, uint32_t now_ms,
                                bool car_semantics, DashState *s);
typedef struct
{
    void *state;
    DashCanDecodeFn decode;
    DashCanExpireFn expire;
} DashCanDialect;

#define DASH_CAN_DIALECT_MAX 2U /* Ford today; RaceCapture is the planned second */
static DashCanDialect g_can_dialects[DASH_CAN_DIALECT_MAX];
static uint8_t g_can_dialect_count = 0U;

/* Call from setup(), before the first drain. Refuses over-capacity and null
 * hooks (returns false) rather than dropping a dialect on the floor. */
static bool dash_can_register_dialect(void *state, DashCanDecodeFn decode,
                                      DashCanExpireFn expire)
{
    if (g_can_dialect_count >= DASH_CAN_DIALECT_MAX ||
        decode == NULL || expire == NULL)
    {
        return false;
    }
    g_can_dialects[g_can_dialect_count].state = state;
    g_can_dialects[g_can_dialect_count].decode = decode;
    g_can_dialects[g_can_dialect_count].expire = expire;
    g_can_dialect_count++;
    return true;
}

/* Staleness pass over every registered dialect: the sketch calls this once
 * per loop right after the drain (KTD6 bench/car semantics live inside each
 * dialect's expire). Defined outside the FDCAN guard on purpose -- on
 * non-FDCAN targets nothing ever becomes CAN-owned, so this is a no-op
 * walk, and the .ino pipeline stays identical across targets. */
static void dash_can_expire_all(uint32_t now_ms, bool car_semantics, DashState *s)
{
    for (uint8_t i = 0U; i < g_can_dialect_count; i++)
    {
        g_can_dialects[i].expire(g_can_dialects[i].state, now_ms,
                                 car_semantics, s);
    }
}

#if defined(ARDUINO_ARCH_STM32) && defined(HAL_FDCAN_MODULE_ENABLED)

static FDCAN_HandleTypeDef g_can1;
static FDCAN_HandleTypeDef g_can2;
static bool g_can_ok = false;  /* both buses up: the cantest gate */
static bool g_can1_ok = false; /* decode bus up: the RX-drain gate -- a dead
                                  bus 2 must not silence ECU decode */

/* FIFO0 depth in elements: sizes the message RAM (RxFifo0ElmtsNbr) AND
 * bounds the drain/flush loops, one macro so "a single drain call can empty
 * a completely full FIFO" stays true by construction rather than by two
 * prose-tied literals agreeing. */
#define DASH_CAN_RX_FIFO_DEPTH 8U

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
    h->Init.RxFifo0ElmtsNbr = DASH_CAN_RX_FIFO_DEPTH; /* also the drain/flush bound */
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

    /* RX filtering diverges per bus (plan 2026-08-15-001 U6):
     *  - FDCAN1 (ECU bus, the decode bus): accept ONLY the Ford 0x270-set
     *    range (0x270-0x278) into FIFO0 and REJECT everything else --
     *    non-matching standard IDs and ALL extended IDs -- so an extended
     *    frame whose 29-bit identifier VALUE reads 0x270 can never reach
     *    the decode FIFO (the drain's IdType check is the second layer).
     *  - FDCAN2 (PMU bus): stays accept-all. `cantest` transmits 0x123 on
     *    bus 1 -- TX is untouched by RX filters -- and receives on bus 2,
     *    so bus 2's accept-all is the half the round-trip depends on;
     *    narrowing bus 1 does not disturb it. */
    const bool decode_bus = (inst == FDCAN1);
    FDCAN_FilterTypeDef f = {0};
    f.IdType = FDCAN_STANDARD_ID;
    f.FilterIndex = 0U;
    f.FilterType = FDCAN_FILTER_RANGE;
    f.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    f.FilterID1 = decode_bus ? DASH_CAN_FORD_ID_ENGINE : 0x000U;
    f.FilterID2 = decode_bus ? DASH_CAN_FORD_ID_TEMPS : 0x7FFU;
    (void)HAL_FDCAN_ConfigFilter(h, &f);
    (void)HAL_FDCAN_ConfigGlobalFilter(h,
                                       decode_bus ? FDCAN_REJECT
                                                  : FDCAN_ACCEPT_IN_RX_FIFO0,
                                       decode_bus ? FDCAN_REJECT
                                                  : FDCAN_ACCEPT_IN_RX_FIFO0,
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
    g_can1_ok = ok1;
    Serial.printf("FDCAN init: ECU=%s PMU=%s (classic 500 kbps)\r\n",
                  ok1 ? "ok" : "FAILED", ok2 ? "ok" : "FAILED");
}

/* ---- Ford-frame RX drain (plan 2026-08-15-001 U6; KTD11 observable) ----
 *
 * Decode-bus counters for the `status` ack. The forced-alarm lesson: a path
 * that can be dead while everything acks ok needs a queryable pulse, and
 * lost-vs-accepted is what separates "quiet bus" from "dropping frames".
 * `accepted` counts frames the decoder CONSUMED, not frames the hardware
 * filter admitted: the 0x270-0x278 range filter also passes IDs the dialect
 * does not speak (0x271-0x273, 0x276-0x277), and counting those would let
 * the pulse read healthy while zero channels decode -- exactly the dead
 * path KTD11 exists to expose. */
static uint32_t g_can_rx_accepted = 0U;       /* frames the Ford decoder consumed */
static uint32_t g_can_rx_lost = 0U;           /* FIFO0 overflow episodes (RF0L), see drain */
static uint32_t g_can_rx_last_accept_ms = 0U; /* millis() stamp of the last consumed frame */

static inline uint32_t dash_can_rx_accepted(void) { return g_can_rx_accepted; }
static inline uint32_t dash_can_rx_lost(void) { return g_can_rx_lost; }
/* ms since the last accepted frame. With accepted still 0 this reads as time
 * since boot (last_accept_ms holds its zero init) -- honest for a bus that
 * has never spoken, and it keeps moving, unlike a frozen counter. */
static inline uint32_t dash_can_rx_ms_since_accept(uint32_t now_ms)
{
    return (uint32_t) (now_ms - g_can_rx_last_accept_ms);
}

/* Drain FDCAN1 FIFO0 into the registered dialect decoders, bounded at the
 * FIFO's own depth.
 * Budget: the 0x270-set aggregates 210 frames/s (100+50+50+10 Hz) against a
 * ~60 Hz loop, so steady state clears ~4 frames per call; the bound
 * (DASH_CAN_RX_FIFO_DEPTH, the same macro that sizes the FIFO in init)
 * lets one call empty a completely full FIFO, and an
 * 8-deep FIFO at 210 frames/s tolerates a ~38 ms loop stall. FIFO0 runs in
 * blocking mode (overwrite disabled), so past that the NEWEST frames drop --
 * RF0L latches, counted below -- and the next fresh frame recovers the
 * stream. Only standard-ID frames reach the decoder: the hardware filter
 * already rejects extended IDs, and the IdType check here is the belt to
 * that suspender (an extended 29-bit identifier VALUE of 0x270 must never
 * pass as a Ford frame -- see dash_can_ford.h's caller contract). */
static void dash_can_rx_drain(uint32_t now_ms, DashState *s)
{
    if (!g_can1_ok)
    {
        return;
    }

    /* RF0L is an IR event flag, not a counter: read + clear counts overflow
     * EPISODES (a lower bound on frames dropped), which is all KTD11 needs.
     * It lives in the interrupt register, so the flag macros are the right
     * API -- HAL_FDCAN_GetProtocolStatus reads the PSR, which does not carry
     * message-lost at all. */
    if (0U != __HAL_FDCAN_GET_FLAG(&g_can1, FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST))
    {
        __HAL_FDCAN_CLEAR_FLAG(&g_can1, FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST);
        g_can_rx_lost++;
    }

    for (uint8_t i = 0U; i < DASH_CAN_RX_FIFO_DEPTH; i++)
    {
        if (0U == HAL_FDCAN_GetRxFifoFillLevel(&g_can1, FDCAN_RX_FIFO0))
        {
            break;
        }
        FDCAN_RxHeaderTypeDef rx;
        /* 64 bytes, not 8: a classic frame legally carries DLC codes 9-15,
         * and HAL_FDCAN_GetRxMessage copies DLCtoBytes[raw DLC code] bytes
         * -- a table that runs to 64 regardless of frame format -- so an
         * 8-byte buffer is a stack smash waiting on one malformed frame. */
        uint8_t data[64] = {0};
        if (HAL_OK != HAL_FDCAN_GetRxMessage(&g_can1, FDCAN_RX_FIFO0, &rx, data))
        {
            break;
        }
        if (FDCAN_STANDARD_ID != rx.IdType)
        {
            continue; /* extended frame: filtered in hardware, guarded again here */
        }
        /* Classic-CAN DLC codes 0-8 ARE the byte count in this HAL
         * (FDCAN_DLC_BYTES_8 == 8U); codes 9-15 mean 8 data bytes on the
         * wire, so clamp before the decoder -- its dlc contract is a byte
         * count, and 9-15 must read as a full frame, not a magic length. */
        const uint8_t dlc = (rx.DataLength <= 8U) ? (uint8_t) rx.DataLength : 8U;
        /* KTD11: count only frames a dialect consumed. The range filter
         * also admits IDs no dialect speaks (0x271-0x273, 0x276-0x277) and
         * runts can arrive on a real bus; stamping those as "accepted"
         * would fake decode health on a quiet dialect. First consumer ends
         * the walk (dialects never share IDs, see the seam comment). */
        for (uint8_t d = 0U; d < g_can_dialect_count; d++)
        {
            if (g_can_dialects[d].decode(g_can_dialects[d].state,
                                         rx.Identifier, dlc, data, now_ms, s))
            {
                g_can_rx_accepted++;
                g_can_rx_last_accept_ms = now_ms;
                break;
            }
        }
    }
}

/* Boot-time flush (setup() calls this once, right before loop() starts):
 * read FIFO0 to empty and clear a latched RF0L, counting NOTHING. Frames
 * that piled up during the ~2.4 s splash predate the first drain, so
 * decoding them would stamp stale data as fresh; the overflow they caused
 * (an 8-deep FIFO fills in ~38 ms at the dialect's 210 frames/s) is a boot
 * artifact, not the bus-health signal the RF0L counter exists to carry. */
static void dash_can_rx_flush(void)
{
    if (!g_can1_ok)
    {
        return;
    }
    for (uint8_t i = 0U; i < DASH_CAN_RX_FIFO_DEPTH; i++)
    {
        if (0U == HAL_FDCAN_GetRxFifoFillLevel(&g_can1, FDCAN_RX_FIFO0))
        {
            break;
        }
        FDCAN_RxHeaderTypeDef rx;
        uint8_t data[64] = {0}; /* 64, not 8: see the drain -- the HAL copies up to 64 bytes */
        if (HAL_OK != HAL_FDCAN_GetRxMessage(&g_can1, FDCAN_RX_FIFO0, &rx, data))
        {
            break;
        }
    }
    __HAL_FDCAN_CLEAR_FLAG(&g_can1, FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST);
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
        uint8_t drop_data[64] = {0}; /* 64, not 8: see the drain -- the HAL copies up to 64 bytes */
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
            uint8_t data[64] = {0}; /* 64, not 8: see the drain -- the HAL copies up to 64 bytes */
            if (HAL_OK == HAL_FDCAN_GetRxMessage(&g_can2, FDCAN_RX_FIFO0, &rx, data))
            {
                if (0x123U != rx.Identifier)
                {
                    /* cantest must coexist with live dialect traffic: the
                     * jumpered pair is one bus, so 0x270-set frames can land
                     * on accept-all bus 2 mid-window. Skip strangers and
                     * keep polling for our echo instead of judging the
                     * first frame that happens to arrive. */
                    continue;
                }
                return (0 == memcmp(payload, data, 8U));
            }
        }
    }
    return false; /* timeout: transceiver, jumper, or termination problem */
}

#else /* non-FDCAN build: CAN hardware lives on the STM32 carrier */

static void dash_can_init(void)
{
}

static bool dash_can_test(void)
{
    return false; /* no FDCAN on this target */
}

/* RX drain no-ops: the dialect decoders still compile (pure headers) and
 * still register, but no frames ever arrive, so the sim keeps every channel
 * and `status` reads can=0,0,0 with only the semantics tag live. */
static void dash_can_rx_drain(uint32_t now_ms, DashState *s)
{
    (void)now_ms;
    (void)s;
}

static void dash_can_rx_flush(void)
{
}

static inline uint32_t dash_can_rx_accepted(void) { return 0U; }
static inline uint32_t dash_can_rx_lost(void) { return 0U; }
static inline uint32_t dash_can_rx_ms_since_accept(uint32_t now_ms)
{
    (void)now_ms;
    return 0U;
}

#endif

#endif /* DASH_CAN_H */
