/*
 * $QNXLicenseC:
 * Copyright 2015, QNX Software Systems.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as
 * contributors under the License or as licensors under other terms.
 * Please review this entire file for other proprietary rights or license
 * notices, as well as the QNX Development Suite License Guide at
 * http://licensing.qnx.com/license-guide/ for other information.
 * $
 */

#include <arm/mx6x.h>
#include "startup.h"
#include <hw/mx6x-can.h>
#include "mini_can.h"
#include <stdbool.h>

/* disabled
#define MDRVR_DEBUG
*/

#define RETRY_TIMEOUT       (20000)
#define NANOSPIN_DELAY      (1000)

/* Macro to get the register bit associated with the mailbox index */
#define MAILBOX(x)                  (1 << x)

/*
 *  Define number of receive mailboxes (TX = 64 - RX)
 *  This value should be the same as the '-r' option to the dev-can-im6x driver
 */
#define NUM_RX_MAILBOX              (CAN_NUM_MAILBOX_USER/2)
#define NUM_TX_MAILBOX              (CAN_NUM_MAILBOX_USER/2)

#define CANMID_DEFAULT              0x00100000

#define TX_INIT_MAILBOX             (NUM_RX_MAILBOX + 1)    /* Last RX mailbox is NUM_RX_MAILBOX , First TX mailbox is NUM_RX_MAILBOX + 1*/

/*
 * Define default bitrate settings for the board.
 * The PSEG1, PSEG2, RJW and PROPSEG values are held constant and the
 * PRESDIV value is changed for the different default bitrates.

 * The FlexCAN module uses CTRL register to set-up the bit timing parameters required by the CAN protocol.
 * Control register (CANCTRL) contains the PROPSEG = PROP_SEG(Bit 0-3),
 * PSEG1 = PHASE_SEG1 (Bit 19-21), PSEG2 = PHASE_SEG2 (Bit 16-18),
 * and the RJW (Bit 22-23) fields which allow the user to configure the bit timing parameters.
 * The prescaler divide register (PRESDIV) allows the user to select the ratio used to derive the clock from the system clock.
 * For the position of the sample point only the relation (SYNC_SEG + PROP_SEG + PHASE_SEG1) / (PHASE_SEG2) is important.
 * The values for PRESDIV, PROPSEG, PSEG1 and PSEG2 are as given below.
 */

#define CAN_RJW                      0
#define CAN_PROPSEG                  0x02
#define CAN_PSEG1                    0x07
#define CAN_PSEG2                    0x01

/*
 * Generated bit rate values by http://www.port.de/engl/canprod/sv_req_form.html
 * Bitrate values for XTAL 24.5 MHz, desired Sample Point at 87.5%
 */
#define CAN_PRESDIV_10K_XTAL         0xAE
#define CAN_PRESDIV_50K_XTAL         0x22
#define CAN_PRESDIV_125K_XTAL        0x0d
#define CAN_PRESDIV_250K_XTAL        0x06

/* Bitrate table for PLL3 30 MHz, desired Sample Point at 87.5% */
#define CAN_PRESDIV_50K_PLL          0x55
#define CAN_PRESDIV_125K_PLL         0x21
#define CAN_PRESDIV_250K_PLL         0x10
#define CAN_PRESDIV_500K_PLL         0x08

/* CAN IRQ's */
#define RINGO_CAN0_SYSINTR           MX6X_CAN1_IRQ
#define RINGO_CAN1_SYSINTR           MX6X_CAN2_IRQ

#define RINGO_CANCTRL2               0x34

#define RINGO_RXMGMASK_RTR           (1 << 31)
#define RINGO_RXMGMASK_IDE           (1 << 30)

/* Not available after kernel starts */
static minican_config_t const * config  = NULL;

/**********************************************************************
 * Function to modify individual register bits.
 *
 * Define as a macro so it can be called safely once we enter the
 * kernel environment. Function prototype:
 *
 * void SET_PORT32(unsigned port, uint32_t mask, uint32_t data)
 **********************************************************************/
#define SET_PORT32(port, mask, data)                   \
{                                                      \
    out32(port, (in32(port) & ~mask) | (data & mask)); \
}

/**********************************************************************
 * Function to modify individual memory bits.
 *
 * Define as a macro so it can be called safely once we enter the
 * kernel environment. Function prototype:
 *
 * void set_mem32(uint32_t *mem, uint32_t mask, uint32_t data)
 **********************************************************************/
#define SET_MEM32(mem, mask, data)          \
{                                           \
    *mem =  (*mem & ~mask) | (data & mask); \
}

static inline __attribute__((always_inline)) uint32_t pack_msg_id(minican_id_t *arb)
{
    if (arb->is_extended)
        return (arb->id & 0x1FFFFFFF);
    else
        return ((arb->id & 0x7FF) << 18);
}

/**********************************************************************
 * Function to transmit a CAN message out the given mailbox.
 **********************************************************************/
static inline __attribute__((always_inline)) void can_tx(struct minican_data *mdata, size_t len, uint8_t dat[8], minican_id_t arb, size_t mailbox)
{
    uint32_t   can_mcf;
    while((in32(mdata->canport + RINGO_CANESR) & RINGO_CANES_IDLE) == 0);
    can_mcf =  mdata->canmem[mailbox].canmcf;
    SET_MEM32(&can_mcf, MSG_BUF_CODE_MASK, MB_CNT_CODE(TRANS_CODE_NOT_READY));
    SET_MEM32(&can_mcf, MB_CNT_RTR, 0);
    SET_MEM32(&can_mcf, MB_CNT_SRR, 0);
    SET_MEM32(&can_mcf, MB_CNT_IDE, (arb.is_extended)? MB_CNT_IDE : 0 );
    SET_MEM32(&can_mcf, MSG_BUF_DLC_MASK, MB_CNT_LENGTH(len));
    mdata->canmem[mailbox].canmcf = can_mcf;
    mdata->canmem[mailbox].canmid = pack_msg_id(&arb);
    mdata->canmem[mailbox].canmdl =   (dat[0] << 24) | (dat[1] << 16)
                                    | (dat[2] <<  8) | (dat[3] <<  0);
    mdata->canmem[mailbox].canmdh =   (dat[4] << 24) | (dat[5] << 16)
                                    | (dat[6] <<  8) | (dat[7] <<  0);
    SET_MEM32(&mdata->canmem[mailbox].canmcf,
        MSG_BUF_CODE_MASK, MB_CNT_CODE(TRANS_CODE_TRANSMIT_ONCE));

    // Errata ERR005829 step 8: Write twice INACTIVE (0x8) twice to first mailbox
    SET_MEM32(&mdata->canmem[CAN_FIRST_MAILBOX_INDEX].canmcf, MSG_BUF_CODE_MASK, MB_CNT_CODE(TRANS_CODE_NOT_READY));
    SET_MEM32(&mdata->canmem[CAN_FIRST_MAILBOX_INDEX].canmcf, MSG_BUF_CODE_MASK, MB_CNT_CODE(TRANS_CODE_NOT_READY));
}

static void inline __attribute__((always_inline)) disable_interrupts(struct minican_data *mdata)
{
    /* Disable all mailbox interrupts (MB0 to MB63) */
    out32(mdata->canport + RINGO_CANIMASK2, 0);
    out32(mdata->canport + RINGO_CANIMASK1, 0);

    /* Bus Off interrupt enabled */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_BOFFMSK, 0);
    /* Error interrupt enabled */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_ERRMASK, 0);
    /* Tx Warning Interrupt enabled */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_TWRNMSK, 0);
    /* Rx Warning Interrupt enabled */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_RWRNMSK, 0);

}

static void inline __attribute__((always_inline)) enable_mbx_interrupts(struct minican_data *mdata)
{
    /* Enable mailbox interrupts (MB1 to MB62) - 1st and last mbox reserved. See Errata ERR005829 */
    out32(mdata->canport + RINGO_CANIMASK2,  0x7FFFFFFF);
    out32(mdata->canport + RINGO_CANIMASK1,  0xFFFFFFFE);

    /* Clear Interrupt Flag by writing it to <91>1<92> */
    out32(mdata->canport + RINGO_CANIFLAG2, 0xffffffff);
    out32(mdata->canport + RINGO_CANIFLAG1, 0xffffffff);
}

#ifdef MDRVR_DEBUG
/* Print CAN device registers */
static void can_print_reg(struct minican_data *mdata)
{
    kprintf("\n******************************************************\n");
    kprintf("CANMCR = 0x%x\t", in32(mdata->canport + RINGO_CANMC));
    kprintf("  CANCTRL = 0x%x\n", in32(mdata->canport + RINGO_CANCTRL));
    kprintf("CANTIMER = 0x%x\t", in32(mdata->canport + RINGO_CANTIMER));
    kprintf("  CANRXGMASK = 0x%x\n", in32(mdata->canport + RINGO_CANRXGMASK));
    kprintf("CANRX14MASK = 0x%x", in32(mdata->canport + RINGO_CANRX14MASK));
    kprintf("  CANRX15MASK = 0x%x\n", in32(mdata->canport + RINGO_CANRX15MASK));
    kprintf("CANECR = 0x%x\t\t", in32(mdata->canport + RINGO_CANECR));
    kprintf("  CANESR = 0x%x\n", in32(mdata->canport + RINGO_CANESR));
    kprintf("CANIMASK2 = 0x%x\t", in32(mdata->canport + RINGO_CANIMASK2));
    kprintf("  CANIMASK1 = 0x%x\n", in32(mdata->canport + RINGO_CANIMASK1));
    kprintf("CANIFLAG2  = 0x%x\t", in32(mdata->canport + RINGO_CANIFLAG2));
    kprintf("  CANIFLAG1 = 0x%x\n", in32(mdata->canport + RINGO_CANIFLAG1));
    kprintf("******************************************************\n");
}
#endif


/**********************************************************************
 * Function to initialize CAN hardware.
 *
 * Don't need to define as a macro since this is only called during Startup.
 *
 * int init_hw(struct minican_data *mdata)
 **********************************************************************/
static int init_hw(struct minican_data *mdata)
{
    int         timeout = RETRY_TIMEOUT, counter = 0, i;
    uint32_t    canmcf_ide = 0, cantest = 0;
    uint8_t     presdiv = CAN_PRESDIV_50K_PLL;
    bool        capture = false, capture_all = false, capture_extended = false;

    if (config)
    {
        switch(config->rate) {
            case MINICAN_BAUD_500K:
                presdiv = CAN_PRESDIV_500K_PLL;
                break;

            case MINICAN_BAUD_250K:
                presdiv = CAN_PRESDIV_250K_PLL;
                break;

            case MINICAN_BAUD_125K:
                presdiv = CAN_PRESDIV_125K_PLL;
                break;

            case MINICAN_BAUD_50K:
                presdiv = CAN_PRESDIV_50K_PLL;
                break;

            default:
                /* Falling back to the pre-configured presdiv */
                break;
        }
    }

    /* Reuse the device structure to set default options */
    CANDEV_RINGO_INIT   devinit =
    {
        .cinit = {
            .devtype = CANDEV_TYPE_RX,
            .can_unit = 0,
            .dev_unit = 0,
            .msgq_size = 100,
            .waitq_size = 16,
        },
        .port =  RINGO_CAN0_REG_BASE,
        .mem =  RINGO_CAN0_MEM_BASE,
        .clk =  RINGO_CAN_CLK_PLL,
        .bitrate =  0,
        .br_presdiv = presdiv,
        .br_propseg = CAN_PROPSEG,
        .br_rjw = CAN_RJW,
        .br_pseg1 = CAN_PSEG1,
        .br_pseg2 = CAN_PSEG2,
        .irqsys = RINGO_CAN0_SYSINTR,
        .flags = 0,
        .numrx = NUM_RX_MAILBOX,
        .numtx = NUM_TX_MAILBOX,
        .midrx = 0x100C0000,
        .midtx = 0x100C0000,
        .timestamp = 0x0,
    };

    mdata->nrx = devinit.numrx;

    /* Map register access to CAN1 device for Startup environment */
    if((mdata->canport = startup_io_map(MX6X_CAN_SIZE, MX6X_CAN1_PORT)) == 0) return(-1);

    /* Map memory access to CAN1 mailbox memory for Startup environment */
    if((mdata->canmem = startup_memory_map(MX6X_CAN_SIZE, MX6X_CAN1_MEM, PROT_READ|PROT_WRITE|PROT_NOCACHE)) == 0) return(-1);

#ifdef MDRVR_DEBUG
    kprintf("mini-driver init_hw: mdata->canport 0x%x\n", mdata->canport);
#endif

    /* Go to INIT mode:
     * Any configuration change/initialization requires that the FlexCAN be frozen by either
     * asserting the HALT bit in the module configuration register or by reset.
     */

    /* Reset Device */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_SOFTRST, RINGO_CANMC_SOFTRST);
    /*
     * Since soft reset is synchronous and has to follow a request/acknowledge procedure
     * across clock domains, it may take some time to fully propagate its effect.
     */
    while((in32(mdata->canport + RINGO_CANMC) & RINGO_CANMC_SOFTRST) != 0)
    {
        if(timeout-- == 0)
        {
#ifdef MDRVR_DEBUG
            kprintf("Enter Init Mode: SOFT_RST timeout!\n");
#endif
            exit(EXIT_FAILURE);
        }
    }

#ifdef MDRVR_DEBUG
    kprintf("-----Enter Init Mode: SOFT_RST ok!\n");
#endif

    /*
     * Go to Disable Mode:
     * The clock source (CLK_SRC bit) should be selected while the module is in Disable Mode.
     * After the clock source is selected and the module is enabled (MDIS bit negated),
     * FlexCAN automatically goes to Freeze Mode.
     */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_MDIS, RINGO_CANMC_MDIS);
    /* Wait for indication that FlexCAN is in Disable Mode */
    while((in32(mdata->canport + RINGO_CANMC) & RINGO_CANMC_LPM_ACK) != RINGO_CANMC_LPM_ACK)
    {
        if(timeout-- == 0)
        {
#ifdef MDRVR_DEBUG
            kprintf("Low Power Mode: LPM_ACK timeout!\n");
#endif
            exit(EXIT_FAILURE);
        }
    }

    /*
     * For some SOC's such as the i.mx6x chips we need to explicitely enable the FlexCAN
     * at this point since Freeze Mode is NOT always automatically enabled.
     */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_MDIS, 0);
    timeout=RETRY_TIMEOUT;
    while((in32(mdata->canport + RINGO_CANMC) & RINGO_CANMC_FRZACK) != RINGO_CANMC_FRZACK)
    {
        if(timeout-- == 0)
        {
#ifdef MDRVR_DEBUG
            kprintf("Unable to enter Freeze Mode\n");
#endif
            exit(EXIT_FAILURE);
        }
    }
#ifdef MDRVR_DEBUG
    kprintf("-----Enter  Freeze  Mode ok!\n");
#endif
    /* Determine the bit timing parameters: PROPSEG, PSEG1, PSEG2, RJW.
     * Determine the bit rate by programming the PRESDIV field.
     * The prescaler divide register (PRESDIV) allows the user to select
     * the ratio used to derive the S-Clock from the system clock.
     * The time quanta clock operates at the S-clock frequency.
     */
    out32(mdata->canport + RINGO_CANCTRL,  (presdiv << RINGO_CANCTRL_PRESDIV_SHIFT) |
                                           (CAN_PROPSEG << RINGO_CANCTRL_PROPSEG_SHIFT) |
                                           (CAN_RJW << RINGO_CANCTRL_RJW_SHIFT) |
                                           (CAN_PSEG1 << RINGO_CANCTRL_PSEG1_SHIFT) |
                                           (CAN_PSEG2 << RINGO_CANCTRL_PSEG2_SHIFT));
    /* Select clock source to be IP bus clock by default */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_CLKSRC, RINGO_CANCTRL_CLKSRC);

    /*
     * For any configuration change/initialization it is required that FlexCAN be put into Freeze Mode.
     * The following is a generic initialization sequence applicable to the FlexCAN module:
     */

    /* 1. Initialize the Module Configuration Register */
    /* Affected registers are in Supervisor memory space */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_SUPV, RINGO_CANMC_SUPV);

    /* Enable the individual filtering per MB and reception queue features by setting the IRMQ bit */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_IRMQ, RINGO_CANMC_IRMQ);

    /* Enable the warning interrupts by setting the WRN_EN bit */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_WRN_EN, RINGO_CANMC_WRN_EN);

    /* If required, disable frame self reception by setting the SRX_DIS bit */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_SRX_DIS, RINGO_CANMC_SRX_DIS);

    /* Enable the local priority feature by setting the LPRIO_EN bit */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_LPRIO_EN, RINGO_CANMC_LPRIO_EN);

    /* Format A One full ID (standard or extended) per filter element */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_IDAM_FormatA, RINGO_CANMC_IDAM_FormatA);

    /* Maximum MBs in use */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_MAXMB_MASK, RINGO_CANMC_MAXMB_MAXVAL);


    /* 2. Initialize the Control Register */
    /* Disable Bus-Off Interrupt */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_BOFFMSK, 0);

    /* Disable Error Interrupt */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_ERRMASK, 0);

    /* Tx Warning Interrupt disabled */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_TWRNMSK, 0);

    /* Rx Warning Interrupt disabled */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_RWRNMSK, 0);

    /* Single Sample Mode should be set by default */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_SAMP, 0);

    /* Disable self-test/loop-back by default */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_LPB, 0);

    /* Disable Timer Sync feature by default */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_TSYNC, 0);

    /* Listen-Only Mode:
     * In listen-only mode, the CAN module is able to receive messages without giving an acknowledgment.
     * Since the module does not influence the CAN bus in this mode  the host device is capable of
     * functioning like a monitor or for automatic bit-rate detection.
     */
    /* De-activate Listen Only Mode by default */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_LOM, 0);

    /* Enable Automatic recovering from Bus Off state */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_BOFFREC, 0);

    /* Determine the internal arbitration mode (LBUF bit) */
    /* The LBUF bit defines the transmit-first scheme.
     * 0 = Message buffer with lowest ID is transmitted first.
     * 1 = Lowest numbered buffer is transmitted first.
     */
    /* Buffer with highest priority is transmitted first */
    SET_PORT32(mdata->canport + RINGO_CANCTRL, RINGO_CANCTRL_LBUF, 0);

    out32(mdata->canport + RINGO_CANECR, 0);
    out32(mdata->canport + RINGO_CANESR, in32(mdata->canport + RINGO_CANESR) | 0xffffffff);
    /* Disable Buffer Interrupt */
    out32(mdata->canport + RINGO_CANIMASK2, 0);
    out32(mdata->canport + RINGO_CANIMASK1, 0);
    /* Clear Interrupt Flags */
    out32(mdata->canport + RINGO_CANIFLAG2, 0xffffffff);
    out32(mdata->canport + RINGO_CANIFLAG1, 0xffffffff);

    /* Initialize CAN mailboxes in device memory */

    if (config && config->msgs_to_rx.buf && (config->msgs_to_rx.count > 0))
    {
        capture = true;

        for (i = 0; i < config->msgs_to_rx.count; i++) {
            if ( config->msgs_to_rx.buf[i].id == CAN_RX_ALL ) {
                capture_all = true;

                if ( config->msgs_to_rx.buf[i].is_extended )
                    capture_extended = true;
            }
        }
    }

    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_IRMQ, RINGO_CANMC_IRMQ);
    SET_PORT32(mdata->canport + RINGO_CANCTRL2, RINGO_CANCTRL2_EACEN, RINGO_CANCTRL2_EACEN);
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_IDAM_FormatD, RINGO_CANMC_IDAM_FormatA);

    if ( capture_all )
    {
        uint32_t mask_reg_val = RINGO_RXMGMASK_RTR;

        if ( !capture_extended )
            mask_reg_val |= RINGO_RXMGMASK_IDE;

        for(i = CAN_FIRST_USER_MAILBOX_INDEX; i <= devinit.numrx; i++)
        {
            /* Disable mailbox to configure message object
             * The control/status word of all message buffers are written
             * as an inactive receive message buffer.
             */
            mdata->canmem[i].canmcf = MB_CNT_CODE(REC_CODE_NOT_ACTIVE);

            out32(mdata->canport + RINGO_CANRXIMR0 + (i * 4), mask_reg_val);

            mdata->canmem[i].canmid = 0;
            mdata->canmem[i].canmdl = 0;
            mdata->canmem[i].canmdh = 0;

            /* Enable mailbox (note IDE is set to 0 here) */
            mdata->canmem[i].canmcf = MB_CNT_CODE(REC_CODE_EMPTY);
        }

        enable_mbx_interrupts(mdata);
    }
    else if ( capture )
    {
        uint32_t mask_reg_val = 0xFFFFFFFF;

        for(i = CAN_FIRST_USER_MAILBOX_INDEX; i <= devinit.numrx; i++)
        {
            size_t msg_list_idx = (i - CAN_FIRST_USER_MAILBOX_INDEX) % config->msgs_to_rx.count;

            /* Disable mailbox to configure message object
             * The control/status word of all message buffers are written
             * as an inactive receive message buffer.
             */
            mdata->canmem[i].canmcf = MB_CNT_CODE(REC_CODE_NOT_ACTIVE);

            out32(mdata->canport + RINGO_CANRXIMR0 + (i * 4), mask_reg_val);

            if ( config->msgs_to_rx.buf[msg_list_idx].is_extended )
                SET_MEM32(&mdata->canmem[i].canmcf, MB_CNT_IDE, MB_CNT_IDE);
            mdata->canmem[i].canmid = pack_msg_id(&config->msgs_to_rx.buf[msg_list_idx]);
            mdata->canmem[i].canmdl = 0;
            mdata->canmem[i].canmdh = 0;

            /* Enable mailbox  */
            SET_MEM32(&mdata->canmem[i].canmcf, MSG_BUF_CODE_MASK, MB_CNT_CODE(REC_CODE_EMPTY));
        }

        enable_mbx_interrupts(mdata);
    }
    else /* No RX IDs specified. Will do the old init of the rx mailboxes, but will mask off the interrupts and not buffer anything. */
    {
        uint32_t mask_reg_val = 0xFFFFFFFF;

        /* Configure Receive Mailboxes */
        counter = 0;
        for(i = CAN_FIRST_USER_MAILBOX_INDEX; i <= devinit.numrx; i++)
        {
            out32(mdata->canport + RINGO_CANRXIMR0 + (i * 4), mask_reg_val);

            /* Disable mailbox to configure message object
             * The control/status word of all message buffers are written
             * as an inactive receive message buffer.
             */
            mdata->canmem[i].canmcf = ((REC_CODE_NOT_ACTIVE & 0x0F) << 24);
            mdata->canmem[i].canmid = 0;
            /* Initialize default receive message ID */
            if (canmcf_ide)
            {   /* Extended frame */
                mdata->canmem[i].canmid = (devinit.midrx + CANMID_DEFAULT * counter++) & RINGO_CANMID_MASK_EXT;
            }
            else
            {   /* Standard frame */
                mdata->canmem[i].canmid = (devinit.midrx + CANMID_DEFAULT * counter++) & RINGO_CANMID_MASK_STD;
            }
            mdata->canmem[i].canmdl = 0x0;
            mdata->canmem[i].canmdh = 0x0;

            /* Enable mailbox */
            mdata->canmem[i].canmcf = ((REC_CODE_EMPTY & 0x0F) << 24) | canmcf_ide;
        }

        disable_interrupts(mdata);
    }

    /* Configure Transmit Mailboxes */
    counter = 0;
    for(i = TX_INIT_MAILBOX; i <= CAN_LAST_USER_MAILBOX_INDEX; i++)
    {
        /* Disable mailbox to configure message object */
        mdata->canmem[i].canmcf = MB_CNT_CODE(TRANS_CODE_NOT_READY) | MB_CNT_LENGTH(CAN_MSG_DATA_MAX) | canmcf_ide;

        /* Initialize default transmit message ID */
        if (canmcf_ide)
        {   /* Extended frame */
            mdata->canmem[i].canmid = (devinit.midtx + CANMID_DEFAULT * counter++) & RINGO_CANMID_MASK_EXT;
        }
        else
        {   /* Standard frame */
            mdata->canmem[i].canmid = (devinit.midtx + CANMID_DEFAULT * counter++) & RINGO_CANMID_MASK_STD;
        }

        mdata->canmem[i].canmdl = 0x0;
        mdata->canmem[i].canmdh = 0x0;
    }

    // Errata ERR005829 step 7: mark first valid maibox as an INACTIVE mailbox
    mdata->canmem[CAN_FIRST_MAILBOX_INDEX].canmcf = (TRANS_CODE_NOT_READY & 0x0F) << 24;
    mdata->canmem[CAN_FIRST_MAILBOX_INDEX].canmid = 0;
    mdata->canmem[CAN_FIRST_MAILBOX_INDEX].canmdl = 0x0;
    mdata->canmem[CAN_FIRST_MAILBOX_INDEX].canmdh = 0x0;

    // Also take the last mailbox out of service. This is not part of the errata
    // but introduced to keep the number of tx and rx mailboxes equal.
    mdata->canmem[CAN_LAST_MAILBOX_INDEX].canmcf = (TRANS_CODE_NOT_READY & 0x0F) << 24;
    mdata->canmem[CAN_LAST_MAILBOX_INDEX].canmid = 0;
    mdata->canmem[CAN_LAST_MAILBOX_INDEX].canmdl = 0x0;
    mdata->canmem[CAN_LAST_MAILBOX_INDEX].canmdh = 0x0;

    /* Enable the FlexCAN module */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_MDIS, 0);
    /* Wait for indication that FlexCAN not in any of the low power modes */
    timeout = RETRY_TIMEOUT;
    while((in32(mdata->canport + RINGO_CANMC) & RINGO_CANMC_LPM_ACK) != 0)
    {
        if(timeout-- == 0)
        {
#ifdef MDRVR_DEBUG
            kprintf("Not able to come out of Low Power Mode: LPM_ACK timeout!\n");
#endif
            exit(EXIT_FAILURE);
        }
    }

    /* Synchronize with can bus */
    SET_PORT32(mdata->canport + RINGO_CANMC, RINGO_CANMC_HALT, 0);

    for (i = 0; i < FLEXCAN_SET_MODE_RETRIES; i++) {
        cantest = in32(mdata->canport + RINGO_CANMC);
        if (!(cantest & (RINGO_CANMC_NOTRDY | RINGO_CANMC_FRZACK))) {
            break;
        }
    }
#ifdef MDRVR_DEBUG
    can_print_reg(mdata);
#endif
    cantest = in32(mdata->canport + RINGO_CANMC);
    if (!(cantest & RINGO_CANMC_FRZACK)) {
#ifdef MDRVR_DEBUG
        kprintf("CAN out from Freezemode\n");
#endif
    } else {
#ifdef MDRVR_DEBUG
        kprintf("ERROR: CAN is stuck in Freezemode\n");
#endif
        return(-1);
    }
    return(0);
}

int mini_can_preconfigure(minican_config_t const * const _config)
{
    config = _config;

    return 0;
}

/**********************************************************************
 * Minidriver Handler Function.
 *
 * int mini_can(int state, void *data)
 **********************************************************************/
int mini_can(int state, void *data)
{
    uint8_t              mbxid = 0;
    uint32_t             flag_rx, flag_tx, i;
    struct minican_data *mdata;
    can_msg_obj_t       *dptr;

    /* Cast the data pointer to our mini-driver data structure */
    mdata = (struct minican_data *)data;

    /* Buffered message data follows the mini-driver data structure */
    dptr = (can_msg_obj_t *) (mdata + 1);

    /* MDRIVER_INTR_ATTACH - Full CAN driver has attached to the interrupt */
    if(state == MDRIVER_INTR_ATTACH)
    {

        disable_interrupts(mdata);

        return(1); /* Minidriver is done and won't be called again */
    }
    /* MDRIVER_INIT - Initialize the CAN hardware and setup the data area */
    else if(state == MDRIVER_INIT)
    {
        #ifdef MDRVR_DEBUG
        kprintf("MDRIVER_INIT\n");
        #endif

        /* Set the Startup poll rate */
        mdriver_max = KILO(16);

        if(init_hw(mdata) == -1)
        {
            /* There was an error, disable the mini-driver */
            #ifdef MDRVR_DEBUG
            kprintf("MDRIVER_INIT init_hw() FAILED\n");
            #endif

            return(1); /* Minidriver is done and won't be called again */
        }
        /* Clear the statistics counters */
        mdata->nstartup = 0;
        mdata->nstartupf = 0;
        mdata->nstartupp = 0;
        mdata->nkernel = 0;
        mdata->nprocess = 0;
        mdata->nrx = 0;
        mdata->tx_enabled = 0;

        /* Transmit out initial messages */
        if(config && config->init_tx_msgs.buf) {
            for (i = 0; (i < NUM_TX_MAILBOX) && (i < config->init_tx_msgs.count); i++) {
                mbxid = TX_INIT_MAILBOX + i;
                /* Set transmit enabled flag */
                SET_MEM32(&mdata->tx_enabled, MAILBOX(i), MAILBOX(i));
                can_tx(mdata, config->init_tx_msgs.buf[i].len, config->init_tx_msgs.buf[i].data, config->init_tx_msgs.buf[i].arb, mbxid);
            }
        }

        return(0); /* Minidriver will be called again */
    }
    /* MDRIVER_STARTUP_PREPARE - Prepare mini-driver for switch from Startup to Kernel environment */
    else if(state == MDRIVER_STARTUP_PREPARE)
    {
        #ifdef MDRVR_DEBUG
        kprintf("MDRIVER_STARTUP_PREPARE MX6X_CAN1_PORT 0x%x, MX6X_CAN1_MEM 0x%x\n", MX6X_CAN1_PORT, MX6X_CAN1_MEM);
        #endif

        mdata->nstartupp = mdata->nstartupp + 1;

        /* Map register access to CAN1 device for Kernel environment */
        if((mdata->canport_k = callout_io_map(MX6X_CAN_SIZE, MX6X_CAN1_PORT)) == 0)
        {
            /* There was an error, disable the mini-driver */
            #ifdef MDRVR_DEBUG
            kprintf("MDRIVER_STARTUP_PREPARE callout_io_map FAILED\n");
            #endif
            /* Disable all interrupts */
            disable_interrupts(mdata);
            return(1); /* Minidriver is done and won't be called again */
        }
        /* Map memory access to CAN1 mailbox memory for Kernel environment */
        if((mdata->canmem_k = callout_memory_map(MX6X_CAN_SIZE, MX6X_CAN1_MEM, PROT_READ|PROT_WRITE|PROT_NOCACHE)) == 0)
        {
            /* There was an error, disable the mini-driver */
            #ifdef MDRVR_DEBUG
            kprintf("MDRIVER_STARTUP_PREPARE callout_memory_map FAILED\n");
            #endif

            /* Disable all interrupts */
            disable_interrupts(mdata);
            return(1); /* Minidriver is done and won't be called again */
        }
    }
    /* MDRIVER_STARTUP_STARTUP - mini-driver called during Startup */
    else if(state == MDRIVER_STARTUP)
    {
        mdata->nstartup = mdata->nstartup + 1;
    }
    else if(state == MDRIVER_KERNEL)
    {
        mdata->nkernel = mdata->nkernel + 1;
        /* Can't call kprintf() once we are called by the kernel */
    }
    else if(state == MDRIVER_PROCESS)
    {
        mdata->nprocess = mdata->nprocess + 1;
        /* Can't call kprintf() once processes are running */
    }

    /*
     * Handle RX interrupts
     */
    flag_rx = in32(mdata->canport + RINGO_CANIFLAG1);

    for (i = 0; (i < (NUM_RX_MAILBOX+1)) && (flag_rx != 0); i++)
    {
        uint32_t mask = (1 << i);

        if ( !(flag_rx & mask) )
        {
            /* this mailbox has no interrupt */
            continue;
        }

        flag_rx &= ~mask;
        mbxid = i;

        while ( (mdata->canmem[mbxid].canmcf & MSG_BUF_CODE_MASK) == MB_CNT_CODE(REC_CODE_BUSY) );

        /* TO DO: Add logic to check if the receive message buffer is full before
         * adding the contents of the next rx mailbox.
         */

        /* Copy data from receive mailbox to receive message buffer */
        dptr[mdata->nrx].canmid = mdata->canmem[mbxid].canmid;
        dptr[mdata->nrx].canmcf = mdata->canmem[mbxid].canmcf;
        dptr[mdata->nrx].canmdh = mdata->canmem[mbxid].canmdh;
        dptr[mdata->nrx].canmdl = mdata->canmem[mbxid].canmdl;

        /* Increment number of buffered receive messages */
        mdata->nrx += 1;

        /* Set mailbox to empty */
        SET_MEM32(&mdata->canmem[mbxid].canmcf, MSG_BUF_CODE_MASK, MB_CNT_CODE(REC_CODE_EMPTY));

        /* Unlock the message buffer by reading the CAN free running timer */
        in32(mdata->canport + RINGO_CANTIMER);

        /* Clear interrupt:
         * Int is cleared when the CPU reads the intrerupt flag register irqsrc
         * while the associated bit is set, and then writes it back as '1'
         * (and no new event of the same type occurs
         * between the read and write action.)
         */
        out32(mdata->canport + RINGO_CANIFLAG1, mask);
    }

    /*
     * Handle TX interrupts
     */
    flag_tx = in32(mdata->canport + RINGO_CANIFLAG2);

    for (i = 0; (i < NUM_TX_MAILBOX) && (flag_tx != 0); i++)
    {
        uint32_t mask = (1 << i);

        if ( !(flag_tx & mask) )
        {
            /* this mailbox has no interrupt */
            continue;
        }

        flag_tx &= ~mask;
        mbxid = TX_INIT_MAILBOX + i;

        /* Clear corresponding tx enabled flag */
        SET_MEM32(&mdata->tx_enabled, MAILBOX(i), 0);

        /* Clear out any code */
        SET_MEM32(&mdata->canmem[mbxid].canmcf, MSG_BUF_CODE_MASK, MB_CNT_CODE(TRANS_CODE_NOT_READY));

        /* Clear interrupt:
         * Int is cleared when the CPU reads the intrerupt flag register irqsrc
         * while the associated bit is set, and then writes it back as '1'
         * (and no new event of the same type occurs
         * between the read and write action.)
         */
        out32(mdata->canport + RINGO_CANIFLAG2, mask);
    }

    /* MDRIVER_STARTUP_FINI - Next time mini-driver is called will be in Kernel environment */
    if(state == MDRIVER_STARTUP_FINI)
    {
        mdata->nstartupf = mdata->nstartupf + 1;

        #ifdef MDRVR_DEBUG
        kprintf("MDRIVER_STARTUP_FINI\n");
        #endif

        /* Switch register and memory mappings from Startup to Kernel environment */
        mdata->canport = mdata->canport_k;
        mdata->canmem = mdata->canmem_k;
    }

    return(0); /* Minidriver will be called again */
}

#if defined(__QNXNTO__) && defined(__USESRCVERSION)
#include <sys/srcversion.h>
__SRCVERSION("$URL: http://svn.ott.qnx.com/product/branches/7.0.0/trunk/hardware/startup/boards/imx6x/mini-can/mini_can.c $ $Rev: 790882 $")
#endif
