/*
Reference Document:
	"IEEE_1149_JTAG_and_Boundary_Scan_Tutorial"
	https://github.com/ARMmbed/DAPLink/blob/master/source/daplink/cmsis-dap/JTAG_DP.c
*/

#include "jtag.h"
#include "timestamp.h"
#include "device.h"
#include "io.h"
#include "vsf.h"

#define DAP_TRANSFER_RnW (1U << 1)
#define DAP_TRANSFER_TIMESTAMP (1U << 7)
#define DAP_TRANSFER_OK (1U << 0)
#define DAP_TRANSFER_WAIT (1U << 1)


#define IO_CFG_INPUT(idx, pin)      (GPIOBANK0->DIR_CLR = (0x1ul << ((idx) * 8 + (pin))))
#define IO_CFG_OUTPUT(idx, pin)     (GPIOBANK0->DIR_SET = (0x1ul << ((idx) * 8 + (pin))))
#define IO_SET(idx, pin)            (GPIODATA0->DT_SET = (0x1ul << ((idx) * 8 + (pin))))
#define IO_CLEAR(idx, pin)          (GPIODATA0->DT_CLR = (0x1ul << ((idx) * 8 + (pin))))
#define IO_GET(idx, pin)            ((GPIODATA0->DT >> ((idx) * 8 + (pin))) & 0x1ul)

#define IO_GET_80_or_00(idx, pin)   ((GPIODATA0->DT & (0x1ul << ((idx) * 8 + (pin)))) ? 0x80 : 0x00)

typedef struct jtag_control_t {
	uint8_t idle;
    uint16_t retry_limit;
    uint16_t delay_tick;

    #if TIMESTAMP_CLOCK
    uint32_t dap_timestamp;
    #endif
    void (*jtag_rw)(uint32_t bits, uint8_t *tms, uint8_t *tdi, uint8_t *tdo);
    void (*jtag_rw_dr)(uint32_t dma_bytes, uint32_t bits_tail, uint8_t *tms, uint8_t *tdi, uint8_t *tdo);
    void (*jtag_delay)(uint16_t delay_tick);
} jtag_control_t;

#if JTAG_COUNT > 0

static jtag_control_t jtag_control;

static void jtag_rw_quick(uint32_t bitlen, uint8_t *tms, uint8_t *tdi, uint8_t *tdo);
static void jtag_rw_slow(uint32_t bitlen, uint8_t *tms, uint8_t *tdi, uint8_t *tdo);
static void jtag_rw_dr_quick(uint32_t bytelen_dma, uint32_t bitlen_tail, uint8_t *tms, uint8_t *tdi, uint8_t *tdo);
static void jtag_rw_dr_slow(uint32_t bytelen_dma, uint32_t bitlen_tail, uint8_t *tms, uint8_t *tdi, uint8_t *tdo);

void vsfhal_jtag_init(int32_t int_priority)
{
    PERIPHERAL_GPIO_TDI_INIT();
    PERIPHERAL_GPIO_TMS_INIT();
    PERIPHERAL_GPIO_TCK_INIT();
    PERIPHERAL_GPIO_TDO_INIT();
    PERIPHERAL_GPIO_SRST_INIT();
    PERIPHERAL_GPIO_TRST_INIT();
    vsfhal_jtag_io_reconfig();

    memset(&jtag_control, 0, sizeof(jtag_control_t));
}

void vsfhal_jtag_fini(void)
{
    PERIPHERAL_GPIO_TDI_FINI();
    PERIPHERAL_GPIO_TMS_FINI();
    PERIPHERAL_GPIO_TCK_FINI();
    PERIPHERAL_GPIO_TDO_FINI();
    PERIPHERAL_GPIO_SRST_FINI();
    PERIPHERAL_GPIO_TRST_FINI();
}

void vsfhal_jtag_io_reconfig(void)
{
    PERIPHERAL_GPIO_TDI_SET_OUTPUT();
    PERIPHERAL_GPIO_TDI_SET();

    PERIPHERAL_GPIO_TMS_SET_OUTPUT();
    PERIPHERAL_GPIO_TMS_SET();
    
    PERIPHERAL_GPIO_TCK_SET_OUTPUT();
    PERIPHERAL_GPIO_TCK_SET();
    
    PERIPHERAL_GPIO_TDO_SET_INPUT();
    
    PERIPHERAL_GPIO_SRST_SET_OUTPUT();
    PERIPHERAL_GPIO_SRST_SET();
    
    PERIPHERAL_GPIO_TRST_SET_OUTPUT();
    PERIPHERAL_GPIO_TRST_SET();
}

#pragma optimize=none
static void delay_jtag_2000khz_1500khz(uint16_t dummy)
{
    __ASM("NOP");
    __ASM("NOP");
    __ASM("NOP");
    __ASM("NOP");
}

#pragma optimize=none
static void delay_jtag_1000khz_750khz(uint16_t dummy)
{
    int32_t temp = 9;
    while (--temp);
}


#pragma optimize=none
static void delay_jtag_500khz_375khz(uint16_t dummy)
{
    int32_t temp = 25;
    while (--temp);
}

#pragma optimize=none
static void delay_jtag_250khz_188khz(uint16_t dummy)
{
    int32_t temp = 57;
    while (--temp);
}

void vsfhal_jtag_config(uint16_t kHz, uint16_t retry, uint8_t idle)
{
    uint32_t temp, apb;
    const vsfhal_clk_info_t *info = vsfhal_clk_info_get();

    jtag_control.idle = idle;
    jtag_control.retry_limit = retry;
    jtag_control.delay_tick = info->ahb_apb_freq_hz / (kHz * 2000);

    if (kHz >= 3000) {
        jtag_control.jtag_rw = jtag_rw_quick;
        jtag_control.jtag_rw_dr = jtag_rw_dr_quick;
        jtag_control.jtag_delay = NULL;
    } else if (kHz >= 1500) {
        jtag_control.jtag_rw = jtag_rw_slow;
        jtag_control.jtag_rw_dr = jtag_rw_dr_slow;
        jtag_control.jtag_delay = delay_jtag_2000khz_1500khz;
    } else if (kHz >= 750) {
        jtag_control.jtag_rw = jtag_rw_slow;
        jtag_control.jtag_rw_dr = jtag_rw_dr_slow;
        jtag_control.jtag_delay = delay_jtag_1000khz_750khz;
    } else if (kHz >= 375) {
        jtag_control.jtag_rw = jtag_rw_slow;
        jtag_control.jtag_rw_dr = jtag_rw_dr_slow;
        jtag_control.jtag_delay = delay_jtag_500khz_375khz;
    } else {
        jtag_control.jtag_rw = jtag_rw_slow;
        jtag_control.jtag_rw_dr = jtag_rw_dr_slow;
        jtag_control.jtag_delay = delay_jtag_250khz_188khz;
    }
}

void vsfhal_jtag_raw(uint32_t bitlen, uint8_t *tms, uint8_t *tdi, uint8_t *tdo)
{
    jtag_rw_slow(bitlen, tms, tdi, tdo);
}

void vsfhal_jtag_ir(uint32_t ir, uint32_t lr_length, uint32_t ir_before, uint32_t ir_after)
{
    uint_fast32_t bitlen;
    uint64_t buf_tms, buf_tdi, buf_tdo;

    buf_tdi = 0;
    lr_length--;

    // Select-DR-Scan, Select-IR-Scan, Capture-IR, Shift-IR
    buf_tms = 0x3;
    bitlen = 4;

    // Bypass before data
    if (ir_before) {
        buf_tdi |= (((uint64_t)0x1 << ir_before) - 1) << bitlen;
        bitlen += ir_before;
    }

    // Set IR bitlen
    if (lr_length) {
        buf_tdi |= (ir & (((uint64_t)0x1 << lr_length) - 1)) << bitlen;
        bitlen += lr_length;
    }

    // Bypass after data
    if (ir_after) {
        buf_tdi |= ((ir >> lr_length) & 0x1) << bitlen;
        bitlen++;
        ir_after--;
        if (ir_after) {
            buf_tdi |= (((uint64_t)0x1 << ir_after) - 1) << bitlen;
            bitlen += ir_after;
        }
        buf_tms |= (uint64_t)0x1 << bitlen;
        buf_tdi |= (uint64_t)0x1 << bitlen;
        bitlen++;
    } else {
        buf_tms |= (uint64_t)0x1 << bitlen;
        buf_tdi |= ((ir >> lr_length) & 0x1) << bitlen;
        bitlen++;
    }

    // Exit1-IR, Update-IR
    buf_tms |= (uint64_t)0x1 << bitlen;
    bitlen++;
    // idle
    buf_tdi |= (uint64_t)0x1 << bitlen;	// keep tdi high
    bitlen++;

    jtag_control.jtag_rw(bitlen, (uint8_t *)&buf_tms, (uint8_t *)&buf_tdi, (uint8_t *)&buf_tdo);
}

/*
Read:	vsfhal_jtag_dr(request, 0, dr_before, dr_after, *read_buf)
Write:	vsfhal_jtag_dr(request, write_value, dr_before, dr_after, NULL)
*/
uint32_t vsfhal_jtag_dr(uint32_t request, uint32_t dr, uint32_t dr_before, uint32_t dr_after, uint8_t *data)
{
    uint_fast32_t ack, retry, dma_bytes, bits_tail, bitlen;
    uint64_t buf_tms, buf_tdi, buf_tdo;

    retry = 0;
    buf_tdi = 0;

    // Select-DR-Scan, Capture-DR, Shift-DR
    buf_tms = 0x1;
    bitlen = 3;

    // Bypass before data
    bitlen += dr_before;

    // RnW, A2, A3
    buf_tdi |= (uint64_t)((request >> 1) & 0x7) << bitlen;
    bitlen += 3;

    // Data Transfer
    if (!(request & DAP_TRANSFER_RnW))
        buf_tdi |= (uint64_t)dr << bitlen;
    bitlen += 31 + dr_after;
    dma_bytes = (bitlen - 8) >> 3;
    buf_tms |= (uint64_t)0x1 << bitlen;
    bitlen++;

    // Update-DR, Idle
    buf_tms |= (uint64_t)0x1 << bitlen;
    bitlen += 1 + jtag_control.idle;
    buf_tdi |= (uint64_t)0x1 << bitlen;	// keep tdi high
    bitlen++;

    bits_tail = bitlen - 8 - (dma_bytes << 3);

    #if TIMESTAMP_CLOCK
    if (request & DAP_TRANSFER_TIMESTAMP)
        jtag_control.dap_timestamp = vsfhal_timestamp_get();
    #endif

    do
    {
        jtag_control.jtag_rw_dr(dma_bytes, bits_tail, (uint8_t *)&buf_tms, (uint8_t *)&buf_tdi, (uint8_t *)&buf_tdo);
        ack = (buf_tdo >> (dr_before + 3)) & 0x7;
        ack = (ack & 0x4) | ((ack & 0x2) >> 1) | ((ack & 0x1) << 1);
        if (ack != DAP_TRANSFER_WAIT)
            break;
    } while (retry++ < jtag_control.retry_limit);

    if (data)
        put_unaligned_le32(buf_tdo >> (dr_before + 6), data);
    return ack;
}

static void jtag_rw_quick(uint32_t bitlen, uint8_t *tms, uint8_t *tdi, uint8_t *tdo)
{
    uint8_t bits, tdi_last, tms_last, tdo_last;

    while (bitlen >= 8) {
        bitlen -= 8;
        bits = 8;
        tms_last = *tms++;
        tdi_last = *tdi++;
        do
        {
            if (tdi_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            if (tms_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            //if (jtag_control.jtag_delay)
            //    jtag_control.jtag_delay(jtag_control.delay_tick);
            tms_last >>= 1;
            tdi_last >>= 1;
            tdo_last >>= 1;
            bits--;
            IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            //if (jtag_control.jtag_delay)
            //    jtag_control.jtag_delay(jtag_control.delay_tick);
            tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
        } while (bits);
        *tdo++ = tdo_last;
    }

    if (bitlen) {
        bits = 8 - bitlen;
        tms_last = *tms;
        tdi_last = *tdi;
        tdo_last = 0;
        do
        {
            if (tdi_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            if (tms_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            //if (jtag_control.jtag_delay)
            //    jtag_control.jtag_delay(jtag_control.delay_tick);
            tms_last >>= 1;
            tdi_last >>= 1;
            tdo_last >>= 1;
            bitlen--;
            IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            //if (jtag_control.jtag_delay)
            //    jtag_control.jtag_delay(jtag_control.delay_tick);
            tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
        } while (bitlen);
        *tdo = tdo_last >> bits;
    }
}
static void jtag_rw_slow(uint32_t bitlen, uint8_t *tms, uint8_t *tdi, uint8_t *tdo)
{
    uint8_t bits, tdi_last, tms_last, tdo_last;

    while (bitlen >= 8) {
        bitlen -= 8;
        bits = 8;
        tms_last = *tms++;
        tdi_last = *tdi++;
        do
        {
            if (tdi_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            if (tms_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            if (jtag_control.jtag_delay)
                jtag_control.jtag_delay(jtag_control.delay_tick);
            tms_last >>= 1;
            tdi_last >>= 1;
            tdo_last >>= 1;
            bits--;
            IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            if (jtag_control.jtag_delay)
                jtag_control.jtag_delay(jtag_control.delay_tick);
            tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
        } while (bits);
        *tdo++ = tdo_last;
    }

    if (bitlen) {
        bits = 8 - bitlen;
        tms_last = *tms;
        tdi_last = *tdi;
        tdo_last = 0;
        do
        {
            if (tdi_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            if (tms_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            if (jtag_control.jtag_delay)
                jtag_control.jtag_delay(jtag_control.delay_tick);
            tms_last >>= 1;
            tdi_last >>= 1;
            tdo_last >>= 1;
            bitlen--;
            IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            if (jtag_control.jtag_delay)
                jtag_control.jtag_delay(jtag_control.delay_tick);
            tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
        } while (bitlen);
        *tdo = tdo_last >> bits;
    }
}

static void jtag_rw_dr_quick(uint32_t bytelen_dma, uint32_t bitlen_tail, uint8_t *tms, uint8_t *tdi, uint8_t *tdo)
{
    uint8_t bits, tdi_last, tms_last, tdo_last;

    // head
    bits = 8;
    tms_last = *tms++;
    tdi_last = *tdi++;
    do
    {
        if (tdi_last & 0x1)
            IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
        else
            IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
        if (tms_last & 0x1)
            IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
        else
            IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
        IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
        //if (jtag_control.jtag_delay)
        //    jtag_control.jtag_delay(jtag_control.delay_tick);
        tms_last >>= 1;
        tdi_last >>= 1;
        tdo_last >>= 1;
        bits--;
        IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
        //if (jtag_control.jtag_delay)
        //    jtag_control.jtag_delay(jtag_control.delay_tick);
        tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
    } while (bits);
    *tdo++ = tdo_last;

    // dma
    #if 0
    do {
        SPI_DATA(JTAG_SPI_BASE) = *tdi;
        tdi++;
        tms++;
        while (SPI_STAT(JTAG_SPI_BASE) & SPI_STAT_TRANS);
        *tdo = SPI_DATA(JTAG_SPI_BASE);
        tdo++;
    } while (--bytelen_dma);
    #endif

    while (bitlen_tail >= 8) {
        bitlen_tail -= 8;
        bits = 8;
        tms_last = *tms++;
        tdi_last = *tdi++;
        do
        {
            if (tdi_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            if (tms_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            //if (jtag_control.jtag_delay)
            //    jtag_control.jtag_delay(jtag_control.delay_tick);
            tms_last >>= 1;
            tdi_last >>= 1;
            tdo_last >>= 1;
            bits--;
            IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            //if (jtag_control.jtag_delay)
            //    jtag_control.jtag_delay(jtag_control.delay_tick);
            tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
        } while (bits);
        *tdo++ = tdo_last;
    }

    if (bitlen_tail) {
        bits = 8 - bitlen_tail;
        tms_last = *tms;
        tdi_last = *tdi;
        tdo_last = 0;
        do
        {
            if (tdi_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            if (tms_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            //if (jtag_control.jtag_delay)
            //    jtag_control.jtag_delay(jtag_control.delay_tick);
            tms_last >>= 1;
            tdi_last >>= 1;
            tdo_last >>= 1;
            bitlen_tail--;
            IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            //if (jtag_control.jtag_delay)
            //    jtag_control.jtag_delay(jtag_control.delay_tick);
            tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
        } while (bitlen_tail);
        *tdo = tdo_last >> bits;
    }
}
static void jtag_rw_dr_slow(uint32_t bytelen_dma, uint32_t bitlen_tail, uint8_t *tms, uint8_t *tdi, uint8_t *tdo)
{
    uint8_t bits, tdi_last, tms_last, tdo_last;

    // head
    bits = 8;
    tms_last = *tms++;
    tdi_last = *tdi++;
    do
    {
        if (tdi_last & 0x1)
            IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
        else
            IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
        if (tms_last & 0x1)
            IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
        else
            IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
        IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
        if (jtag_control.jtag_delay)
            jtag_control.jtag_delay(jtag_control.delay_tick);
        tms_last >>= 1;
        tdi_last >>= 1;
        tdo_last >>= 1;
        bits--;
        IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
        if (jtag_control.jtag_delay)
            jtag_control.jtag_delay(jtag_control.delay_tick);
        tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
    } while (bits);
    *tdo++ = tdo_last;

    // dma
    #if 0
    do {
        SPI_DATA(JTAG_SPI_BASE) = *tdi;
        tdi++;
        tms++;
        while (SPI_STAT(JTAG_SPI_BASE) & SPI_STAT_TRANS);
        *tdo = SPI_DATA(JTAG_SPI_BASE);
        tdo++;
    } while (--bytelen_dma);
    #endif

    while (bitlen_tail >= 8) {
        bitlen_tail -= 8;
        bits = 8;
        tms_last = *tms++;
        tdi_last = *tdi++;
        do
        {
            if (tdi_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            if (tms_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            if (jtag_control.jtag_delay)
                jtag_control.jtag_delay(jtag_control.delay_tick);
            tms_last >>= 1;
            tdi_last >>= 1;
            tdo_last >>= 1;
            bits--;
            IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            if (jtag_control.jtag_delay)
                jtag_control.jtag_delay(jtag_control.delay_tick);
            tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
        } while (bits);
        *tdo++ = tdo_last;
    }

    if (bitlen_tail) {
        bits = 8 - bitlen_tail;
        tms_last = *tms;
        tdi_last = *tdi;
        tdo_last = 0;
        do
        {
            if (tdi_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TDI_IDX, PERIPHERAL_GPIO_TDI_PIN);
            if (tms_last & 0x1)
                IO_SET(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            else
                IO_CLEAR(PERIPHERAL_GPIO_TMS_MO_IDX, PERIPHERAL_GPIO_TMS_MO_PIN);
            IO_CLEAR(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            if (jtag_control.jtag_delay)
                jtag_control.jtag_delay(jtag_control.delay_tick);
            tms_last >>= 1;
            tdi_last >>= 1;
            tdo_last >>= 1;
            bitlen_tail--;
            IO_SET(PERIPHERAL_GPIO_TCK_JTAG_IDX, PERIPHERAL_GPIO_TCK_JTAG_PIN);
            if (jtag_control.jtag_delay)
                jtag_control.jtag_delay(jtag_control.delay_tick);
            tdo_last |= IO_GET_80_or_00(PERIPHERAL_GPIO_TDO_IDX, PERIPHERAL_GPIO_TDO_PIN);
        } while (bitlen_tail);
        *tdo = tdo_last >> bits;
    }
}

#if TIMESTAMP_CLOCK
uint32_t vsfhal_jtag_get_timestamp(void)
{
    return jtag_control.dap_timestamp;
}
#endif

#endif
