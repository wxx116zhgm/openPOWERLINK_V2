/****************************************************************************

  (c) SYSTEC electronic GmbH, D-07973 Greiz, August-Bebel-Str. 29
      www.systec-electronic.com

  Project:      openPOWERLINK

  Description:  Ethernet driver for Intel 82573 Gigabit Ethernet Controller
                and compatible.

  License:

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

    3. Neither the name of SYSTEC electronic GmbH nor the names of its
       contributors may be used to endorse or promote products derived
       from this software without prior written permission. For written
       permission, please contact info@systec-electronic.com.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.

    Severability Clause:

        If a provision of this License is or becomes illegal, invalid or
        unenforceable in any jurisdiction, that shall not affect:
        1. the validity or enforceability in that jurisdiction of any other
           provision of this License; or
        2. the validity or enforceability in other jurisdictions of that or
           any other provision of this License.

  -------------------------------------------------------------------------

                $RCSfile$

                $Author$

                $Revision$  $Date$

                $State$

                Build Environment:
                Dev C++ and GNU-Compiler for m68k

  -------------------------------------------------------------------------

  Revision History:

  2009/12/08 m.u.:   start of implementation

****************************************************************************/

#include "global.h"
#include "EplInc.h"
#include "edrv.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/version.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <asm/irq.h>
#include <linux/sched.h>
#include <linux/delay.h>


/***************************************************************************/
/*                                                                         */
/*                                                                         */
/*          G L O B A L   D E F I N I T I O N S                            */
/*                                                                         */
/*                                                                         */
/***************************************************************************/

// Buffer handling:
// All buffers are created statically (i.e. at compile time resp. at
// initialisation via kmalloc() ) and not dynamically on request (i.e. via
// EdrvAllocTxMsgBuffer().
// EdrvAllocTxMsgBuffer() searches for an unused buffer which is large enough.
// EdrvInit() may allocate some buffers with sizes less than maximum frame
// size (i.e. 1514 bytes), e.g. for SoC, SoA, StatusResponse, IdentResponse,
// NMT requests / commands. The less the size of the buffer the less the
// number of the buffer.


//---------------------------------------------------------------------------
// const defines
//---------------------------------------------------------------------------

#ifndef EDRV_MAX_TX_BUFFERS
#define EDRV_MAX_TX_BUFFERS     42
#endif

#ifndef EDRV_MAX_TX_DESCS
#define EDRV_MAX_TX_DESCS       16
#endif

#ifndef EDRV_MAX_RX_DESCS
#define EDRV_MAX_RX_DESCS       16
#endif

#define EDRV_MAX_FRAME_SIZE     0x600

#define EDRV_TX_BUFFER_SIZE     (EDRV_MAX_TX_BUFFERS * EDRV_MAX_FRAME_SIZE) // n * (MTU + 14 + 4)
#define EDRV_TX_DESCS_SIZE      (EDRV_MAX_TX_DESCS * sizeof (tEdrvTxDesc))

#define EDRV_RX_BUFFER_PER_DESC_SIZE 2048
#define EDRV_RX_BUFFER_SIZE     (EDRV_MAX_RX_DESCS * EDRV_RX_BUFFER_PER_DESC_SIZE)
#define EDRV_RX_DESCS_SIZE      (EDRV_MAX_RX_DESCS * sizeof (tEdrvRxDesc))

#define EDRV_AUTO_READ_DONE_TIMEOUT 10  // ms
#define EDRV_MASTER_DISABLE_TIMEOUT 90  // ms


#define DRV_NAME                "epl"

// Tx descriptor definitions
#define EDRV_TX_DESC_CMD_RS     0x08000000  // Report Status
#define EDRV_TX_DESC_CMD_IFCS   0x02000000  // Insert Frame Check Sum
#define EDRV_TX_DESC_CMD_EOP    0x01000000  // End of Packet
#define EDRV_TX_DESC_CMD_DEF    (EDRV_TX_DESC_CMD_EOP \
                               | EDRV_TX_DESC_CMD_IFCS \
                               | EDRV_TX_DESC_CMD_RS)
#define EDRV_TX_DESC_STATUS_DD  0x01        // Descriptor Done
#define EDRV_TX_DESC_STATUS_EC  0x02        // Excess Collisions
#define EDRV_TX_DESC_STATUS_LC  0x04        // Late Collision


// Ethernet Controller Register Definitions
#define EDRV_REGDW_CTRL         0x00000     // Device Control
#define EDRV_REGDW_CTRL_FD      0x00000001  // Full-Duplex
#define EDRV_REGDW_CTRL_MST_DIS 0x00000004  // GIO Master Disable
#define EDRV_REGDW_CTRL_LRST    0x00000008  // Link Reset
#define EDRV_REGDW_CTRL_SLU     0x00000040  // Set Link Up
#define EDRV_REGDW_CTRL_RST     0x04000000  // Reset
#define EDRV_REGDW_CTRL_PHY_RST 0x80000000  // PHY Reset

#define EDRV_REGDW_CTRL_DEF     (EDRV_REGDW_CTRL_LRST \
                               | EDRV_REGDW_CTRL_SLU)

#define EDRV_REGDW_STATUS       0x00008     // Device Status
#define EDRV_REGDW_STATUS_MST_EN 0x00080000 // GIO Master Enable Status

#define EDRV_REGDW_EEC          0x00010     // EEPROM Control Register
#define EDRV_REGDW_EEC_AUTO_RD  0x00000200  // Auto Read Done

#define EDRV_REGDW_ICR          0x000C0     // Interrupt Cause Read
#define EDRV_REGDW_IMS          0x000D0     // Interrupt Mask Set/Read
#define EDRV_REGDW_IMC          0x000D8     // Interrupt Mask Clear
#define EDRV_REGDW_INT_MASK_ALL 0xFFFFFFFF  // mask all interrupts
#define EDRV_REGDW_INT_TXDW     0x00000001  // Transmit Descriptor Written Back
#define EDRV_REGDW_INT_TXQE     0x00000002  // Transmit Descriptor Queue Empty
#define EDRV_REGDW_INT_LSC      0x00000004  // Link Status Change
#define EDRV_REGDW_INT_RXSEQ    0x00000008  // Receive Sequence Error
#define EDRV_REGDW_INT_RXT0     0x00000080  // Receiver Timer Interrupt
#define EDRV_REGDW_INT_RXDMT0   0x00000010  // Receive Descriptor Minimum Threshold Reached
#define EDRV_REGDW_INT_RXO      0x00000040  // Receiver Overrun
#define EDRV_REGDW_INT_TXD_LOW  0x00008000  // Transmit Descriptor Low Threshold hit
#define EDRV_REGDW_INT_SRPD     0x00010000  // Small Receive Packet Detected
#define EDRV_REGDW_INT_INT_ASSERTED 0x80000000  // PCIe Int. has been asserted

#define EDRV_REGDW_INT_MASK_DEF (EDRV_REGDW_INT_TXDW \
                               | EDRV_REGDW_INT_RXT0 \
                               | EDRV_REGDW_INT_RXDMT0 \
                               | EDRV_REGDW_INT_RXO \
                               | EDRV_REGDW_INT_RXSEQ)

#define EDRV_REGDW_TIPG         0x000410    // Transmit Inter Packet Gap
#define EDRV_REGDW_TIPG_DEF     0x00702008  // default according to Intel PCIe GbE Controllers Open Source Software Developer's Manual

#define EDRV_REGDW_TXDCTL       0x003828    // Transmit Descriptor Control
#define EDRV_REGDW_TXDCTL_GRAN  0x01000000  // Granularity (1=Descriptor, 0=Cache line)
//#define EDRV_REGDW_TXDCTL_WTHRESH 0x01000000  // Write Back Threshold
#define EDRV_REGDW_TXDCTL_DEF   (EDRV_REGDW_TXDCTL_GRAN)

#define EDRV_REGDW_RXDCTL       0x002828    // Receive Descriptor Control
#define EDRV_REGDW_RXDCTL_GRAN  0x01000000  // Granularity (1=Descriptor, 0=Cache line)
#define EDRV_REGDW_RXDCTL_DEF   (EDRV_REGDW_RXDCTL_GRAN)

#define EDRV_REGDW_TCTL         0x000400    // Transmit Control
#define EDRV_REGDW_TCTL_EN      0x00000002  // Transmit Enable
#define EDRV_REGDW_TCTL_PSP     0x00000008  // Pad Short Packets
#define EDRV_REGDW_TCTL_CT      0x000000F0  // Collision Threshold
#define EDRV_REGDW_TCTL_COLD    0x0003F000  // Collision Distance
#define EDRV_REGDW_TCTL_DEF     (EDRV_REGDW_TCTL_EN \
                               | EDRV_REGDW_TCTL_PSP \
                               | EDRV_REGDW_TCTL_CT \
                               | EDRV_REGDW_TCTL_COLD)

#define EDRV_REGDW_RCTL         0x000100    // Receive Control
#define EDRV_REGDW_RCTL_EN      0x00000002  // Receive Enable
#define EDRV_REGDW_RCTL_BAM     0x00008000  // Broadcast Accept Mode
#define EDRV_REGDW_RCTL_SECRC   0x04000000  // Strip Ethernet CRC (do not store in host memory)

#define EDRV_REGDW_RCTL_BSIZE_2048  0x00000000  // buffer size is 2048 byte

#define EDRV_REGDW_RCTL_DEF     (EDRV_REGDW_RCTL_EN \
                               | EDRV_REGDW_RCTL_BAM \
                               | EDRV_REGDW_RCTL_SECRC \
                               | EDRV_REGDW_RCTL_BSIZE_2048)

#define EDRV_REGDW_TDBAL        0x003800    // Transmit Descriptor Base Adress Low
#define EDRV_REGDW_TDBAH        0x003804    // Transmit Descriptor Base Adress High
#define EDRV_REGDW_TDLEN        0x003808    // Transmit Descriptor Length
#define EDRV_REGDW_TDH          0x003810    // Transmit Descriptor Head
#define EDRV_REGDW_TDT          0x003818    // Transmit Descriptor Tail

#define EDRV_REGDW_RDBAL0       0x002800    // Receive Descriptor Base Adress Low
#define EDRV_REGDW_RDBAH0       0x002804    // Receive Descriptor Base Adress High
#define EDRV_REGDW_RDLEN0       0x002808    // Receive Descriptor Length
#define EDRV_REGDW_RDH0         0x002810    // Receive Descriptor Head
#define EDRV_REGDW_RDT0         0x002818    // Receive Descriptor Tail

#define EDRV_REGDW_MTA(n)       (0x05200 + 4*n)  // Multicast Table Array

#define EDRV_REGDW_RAL(n)       (0x05400 + 8*n)  // Receive Address Low
#define EDRV_REGDW_RAH(n)       (0x05404 + 8*n)  // Receive Address HIGH
#define EDRV_REGDW_RAH_AV       0x80000000  // Receive Address Valid

// defines for the status byte in the receive descriptor
#define EDRV_RXSTAT_DD            0x01    // Descriptor Done (Processed by Hardware)
#define EDRV_RXSTAT_EOP           0x02    // End of Packet
#define EDRV_RXERR_CE             0x01    // CRC Error
#define EDRV_RXERR_SEQ            0x04    // Sequence Error


#define EDRV_REGDW_WRITE(dwReg, dwVal)  writel(dwVal, EdrvInstance_l.m_pIoAddr + dwReg)
#define EDRV_REGDW_READ(dwReg)          readl(EdrvInstance_l.m_pIoAddr + dwReg)


// TracePoint support for realtime-debugging
#ifdef _DBG_TRACE_POINTS_
    void  PUBLIC  TgtDbgSignalTracePoint (BYTE bTracePointNumber_p);
    void  PUBLIC  TgtDbgPostTraceValue (DWORD dwTraceValue_p);
    #define TGT_DBG_SIGNAL_TRACE_POINT(p)   TgtDbgSignalTracePoint(p)
    #define TGT_DBG_POST_TRACE_VALUE(v)     TgtDbgPostTraceValue(v)
#else
    #define TGT_DBG_SIGNAL_TRACE_POINT(p)
    #define TGT_DBG_POST_TRACE_VALUE(v)
#endif

#define EDRV_COUNT_SEND                 TGT_DBG_SIGNAL_TRACE_POINT(2)
#define EDRV_COUNT_TIMEOUT              TGT_DBG_SIGNAL_TRACE_POINT(3)
#define EDRV_COUNT_PCI_ERR              TGT_DBG_SIGNAL_TRACE_POINT(4)
#define EDRV_COUNT_TX                   TGT_DBG_SIGNAL_TRACE_POINT(5)
#define EDRV_COUNT_RX                   TGT_DBG_SIGNAL_TRACE_POINT(6)
#define EDRV_COUNT_LATECOLLISION        TGT_DBG_SIGNAL_TRACE_POINT(10)
#define EDRV_COUNT_TX_COL_RL            TGT_DBG_SIGNAL_TRACE_POINT(11)
#define EDRV_COUNT_TX_FUN               TGT_DBG_SIGNAL_TRACE_POINT(12)
#define EDRV_COUNT_TX_ERR               TGT_DBG_SIGNAL_TRACE_POINT(13)
#define EDRV_COUNT_RX_CRC               TGT_DBG_SIGNAL_TRACE_POINT(14)
#define EDRV_COUNT_RX_ERR               TGT_DBG_SIGNAL_TRACE_POINT(15)
#define EDRV_COUNT_RX_SEQ               TGT_DBG_SIGNAL_TRACE_POINT(16)
#define EDRV_COUNT_RX_ORUN              TGT_DBG_SIGNAL_TRACE_POINT(17)
#define EDRV_COUNT_RX_ALL               TGT_DBG_SIGNAL_TRACE_POINT(18)

#define EDRV_TRACE_CAPR(x)              TGT_DBG_POST_TRACE_VALUE(((x) & 0xFFFF) | 0x06000000)
#define EDRV_TRACE_RX_CRC(x)            TGT_DBG_POST_TRACE_VALUE(((x) & 0xFFFF) | 0x0E000000)
#define EDRV_TRACE_RX_ERR(x)            TGT_DBG_POST_TRACE_VALUE(((x) & 0xFFFF) | 0x0F000000)
#define EDRV_TRACE_RX_PUN(x)            TGT_DBG_POST_TRACE_VALUE(((x) & 0xFFFF) | 0x11000000)
#define EDRV_TRACE(x)                   TGT_DBG_POST_TRACE_VALUE(((x) & 0xFFFF0000) | 0x0000FEC0)


//---------------------------------------------------------------------------
// local types
//---------------------------------------------------------------------------

typedef struct
{
    QWORD               m_le_qwBufferAddr;
    DWORD               m_le_dwLengthCmd;
    DWORD               m_le_dwStatus;

} tEdrvTxDesc;

typedef struct
{
    QWORD               m_le_qwBufferAddr;
    WORD                m_le_wLength;
    WORD                m_le_wChecksum;
    BYTE                m_bStatus;
    BYTE                m_bError;
    WORD                m_le_wReserved;

} tEdrvRxDesc;


// Private structure
typedef struct
{
    struct pci_dev*     m_pPciDev;      // pointer to PCI device structure
    void*               m_pIoAddr;      // pointer to register space of Ethernet controller
    BYTE*               m_pbRxBuf;      // pointer to Rx buffer
    dma_addr_t          m_pRxBufDma;
    tEdrvRxDesc*        m_pRxDesc;      // pointer to Rx descriptors
    dma_addr_t          m_pRxDescDma;
    BYTE*               m_pbTxBuf;      // pointer to Tx buffer
    dma_addr_t          m_pTxBufDma;
    tEdrvTxDesc*        m_pTxDesc;      // pointer to Tx descriptors
    tEdrvTxBuffer*      m_apTxBuffer[EDRV_MAX_TX_DESCS];
    dma_addr_t          m_pTxDescDma;
    BOOL                m_afTxBufUsed[EDRV_MAX_TX_BUFFERS];
    unsigned int        m_uiHeadTxDesc;
    unsigned int        m_uiTailTxDesc;
    unsigned int        m_uiHeadRxDesc;
    unsigned int        m_uiTailRxDesc;

    tEdrvInitParam      m_InitParam;

} tEdrvInstance;



//---------------------------------------------------------------------------
// local function prototypes
//---------------------------------------------------------------------------

static int EdrvInitOne(struct pci_dev *pPciDev,
                       const struct pci_device_id *pId);

static void EdrvRemoveOne(struct pci_dev *pPciDev);


//---------------------------------------------------------------------------
// module global vars
//---------------------------------------------------------------------------
// buffers and buffer descriptors and pointers

static struct pci_device_id aEdrvPciTbl[] = {
    {0x8086, 0x109a, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
    {0,}
};
MODULE_DEVICE_TABLE (pci, aEdrvPciTbl);


static tEdrvInstance EdrvInstance_l;


static struct pci_driver EdrvDriver = {
    .name         = DRV_NAME,
    .id_table     = aEdrvPciTbl,
    .probe        = EdrvInitOne,
    .remove       = EdrvRemoveOne,
};




/***************************************************************************/
/*                                                                         */
/*                                                                         */
/*          C L A S S  <edrv>                                              */
/*                                                                         */
/*                                                                         */
/***************************************************************************/
//
// Description:
//
//
/***************************************************************************/


//=========================================================================//
//                                                                         //
//          P R I V A T E   D E F I N I T I O N S                          //
//                                                                         //
//=========================================================================//

//---------------------------------------------------------------------------
// const defines
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// local types
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// local vars
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// local function prototypes
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
//
// Function:    EdrvInit
//
// Description: function for init of the Ethernet controller
//
// Parameters:  pEdrvInitParam_p    = pointer to struct including the init-parameters
//
// Returns:     Errorcode           = kEplSuccessful
//                                  = kEplNoResource
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvInit(tEdrvInitParam * pEdrvInitParam_p)
{
tEplKernel  Ret;
int         iResult;
int         iIndex;

    Ret = kEplSuccessful;

    // clear instance structure
    EPL_MEMSET(&EdrvInstance_l, 0, sizeof (EdrvInstance_l));

    // save the init data
    EdrvInstance_l.m_InitParam = *pEdrvInitParam_p;

    // clear driver structure
    EPL_MEMSET(&EdrvDriver, 0, sizeof (EdrvDriver));
    EdrvDriver.name         = DRV_NAME,
    EdrvDriver.id_table     = aEdrvPciTbl,
    EdrvDriver.probe        = EdrvInitOne,
    EdrvDriver.remove       = EdrvRemoveOne,

    // register PCI driver
    iResult = pci_register_driver (&EdrvDriver);
    if (iResult != 0)
    {
        printk("%s pci_register_driver failed with %d\n", __FUNCTION__, iResult);
        Ret = kEplNoResource;
        goto Exit;
    }

    if (EdrvInstance_l.m_pPciDev == NULL)
    {
        printk("%s m_pPciDev=NULL\n", __FUNCTION__);
        Ret = EdrvShutdown();
        Ret = kEplNoResource;
        goto Exit;
    }

    // local MAC address might have been changed in EdrvInitOne
    EPL_MEMCPY(pEdrvInitParam_p->m_abMyMacAddr, EdrvInstance_l.m_InitParam.m_abMyMacAddr, 6);

    printk("%s local MAC = ", __FUNCTION__);
    for (iIndex = 0; iIndex < 6; iIndex++)
    {
        printk("%02X ", (unsigned int)pEdrvInitParam_p->m_abMyMacAddr[iIndex]);
    }
    printk("\n");

Exit:
    return Ret;

}


//---------------------------------------------------------------------------
//
// Function:    EdrvShutdown
//
// Description: Shutdown the Ethernet controller
//
// Parameters:  void
//
// Returns:     Errorcode   = kEplSuccessful
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvShutdown(void)
{

    // unregister PCI driver
    printk("%s calling pci_unregister_driver()\n", __FUNCTION__);
    pci_unregister_driver (&EdrvDriver);

    return kEplSuccessful;
}


//---------------------------------------------------------------------------
//
// Function:    EdrvDefineRxMacAddrEntry
//
// Description: Set a multicast entry into the Ethernet controller
//
// Parameters:  pbMacAddr_p     = pointer to multicast entry to set
//
// Returns:     Errorcode       = kEplSuccessful
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvDefineRxMacAddrEntry (BYTE * pbMacAddr_p)
{
tEplKernel  Ret = kEplSuccessful;
DWORD       dwData;
int         iIndex;

    // entry 0 is used for local MAC address

    for (iIndex = 1; iIndex < 16; iIndex++)
    {
        dwData = EDRV_REGDW_READ(EDRV_REGDW_RAH(iIndex));
        if ((dwData & EDRV_REGDW_RAH_AV) == 0)
        {   // free MAC address entry
            break;
        }
    }

    if (iIndex == 16)
    {   // no free entry found
        printk("%s Implementation of Multicast Table Array support required\n", __FUNCTION__);
        Ret = kEplEdrvInitError;
        goto Exit;
    }
    else
    {   
        // write MAC address to free entry
        dwData = 0;
        dwData |= pbMacAddr_p[0] <<  0;
        dwData |= pbMacAddr_p[1] <<  8;
        dwData |= pbMacAddr_p[2] << 16;
        dwData |= pbMacAddr_p[3] << 24;
        EDRV_REGDW_WRITE(EDRV_REGDW_RAL(iIndex), dwData);
        dwData = 0;
        dwData |= pbMacAddr_p[4] <<  0;
        dwData |= pbMacAddr_p[5] <<  8;
        dwData |= EDRV_REGDW_RAH_AV;
        EDRV_REGDW_WRITE(EDRV_REGDW_RAH(iIndex), dwData);
    }

Exit:

    return Ret;
}


//---------------------------------------------------------------------------
//
// Function:    EdrvUndefineRxMacAddrEntry
//
// Description: Reset a multicast entry in the Ethernet controller
//
// Parameters:  pbMacAddr_p     = pointer to multicast entry to reset
//
// Returns:     Errorcode       = kEplSuccessful
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvUndefineRxMacAddrEntry (BYTE * pbMacAddr_p)
{
tEplKernel  Ret = kEplSuccessful;
DWORD       dwData;
int         iIndex;
DWORD       dwAddrLow;
DWORD       dwAddrHigh;

    dwAddrLow   = 0;
    dwAddrLow  |= pbMacAddr_p[0] <<  0;
    dwAddrLow  |= pbMacAddr_p[1] <<  8;
    dwAddrLow  |= pbMacAddr_p[2] << 16;
    dwAddrLow  |= pbMacAddr_p[3] << 24;
    dwAddrHigh  = 0;
    dwAddrHigh |= pbMacAddr_p[4] <<  0;
    dwAddrHigh |= pbMacAddr_p[5] <<  8;
    dwAddrHigh |= EDRV_REGDW_RAH_AV;

    for (iIndex = 1; iIndex < 16; iIndex++)
    {
        dwData = EDRV_REGDW_READ(EDRV_REGDW_RAH(iIndex));
        if ((dwData & (EDRV_REGDW_RAH_AV | 0xFFFF)) == dwAddrHigh)
        {
            dwData = EDRV_REGDW_READ(EDRV_REGDW_RAL(iIndex));
            if (dwData == dwAddrLow)
            {   // set address valid bit to invalid
                EDRV_REGDW_WRITE(EDRV_REGDW_RAH(iIndex), 0);
                break;
            }
        }
    }

    return Ret;
}

tEplKernel EdrvChangeFilter(tEdrvFilter*    pFilter_p,
                            unsigned int    uiCount_p,
                            unsigned int    uiEntryChanged_p,
                            unsigned int    uiChangeFlags_p)
{
tEplKernel      Ret = kEplSuccessful;

    return Ret;
}


//---------------------------------------------------------------------------
//
// Function:    EdrvAllocTxMsgBuffer
//
// Description: Register a Tx-Buffer
//
// Parameters:  pBuffer_p   = pointer to Buffer structure
//
// Returns:     Errorcode   = kEplSuccessful
//                          = kEplEdrvNoFreeBufEntry
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvAllocTxMsgBuffer       (tEdrvTxBuffer * pBuffer_p)
{
tEplKernel Ret = kEplSuccessful;
DWORD i;

    if (pBuffer_p->m_uiMaxBufferLen > EDRV_MAX_FRAME_SIZE)
    {
        Ret = kEplEdrvNoFreeBufEntry;
        goto Exit;
    }

    if (EdrvInstance_l.m_pbTxBuf == NULL)
    {
        printk("%s Tx buffers currently not allocated\n", __FUNCTION__);
        Ret = kEplEdrvNoFreeBufEntry;
        goto Exit;
    }

    // search a free Tx buffer with appropriate size
    for (i = 0; i < EDRV_MAX_TX_BUFFERS; i++)
    {
        if (EdrvInstance_l.m_afTxBufUsed[i] == FALSE)
        {
            // free channel found
            EdrvInstance_l.m_afTxBufUsed[i] = TRUE;
            pBuffer_p->m_BufferNumber.m_dwVal = i;
            pBuffer_p->m_pbBuffer = EdrvInstance_l.m_pbTxBuf + (i * EDRV_MAX_FRAME_SIZE);
            pBuffer_p->m_uiMaxBufferLen = EDRV_MAX_FRAME_SIZE;
            break;
        }
    }
    if (i >= EDRV_MAX_TX_BUFFERS)
    {
        Ret = kEplEdrvNoFreeBufEntry;
        goto Exit;
    }

Exit:
    return Ret;

}


//---------------------------------------------------------------------------
//
// Function:    EdrvReleaseTxMsgBuffer
//
// Description: Register a Tx-Buffer
//
// Parameters:  pBuffer_p   = pointer to Buffer structure
//
// Returns:     Errorcode   = kEplSuccessful
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvReleaseTxMsgBuffer     (tEdrvTxBuffer * pBuffer_p)
{
unsigned int uiBufferNumber;

    uiBufferNumber = pBuffer_p->m_BufferNumber.m_dwVal;

    if (uiBufferNumber < EDRV_MAX_TX_BUFFERS)
    {
        EdrvInstance_l.m_afTxBufUsed[uiBufferNumber] = FALSE;
    }

    return kEplSuccessful;

}


//---------------------------------------------------------------------------
//
// Function:    EdrvSendTxMsg
//
// Description: immediately starts the transmission of the buffer
//
// Parameters:  pBuffer_p   = buffer descriptor to transmit
//
// Returns:     Errorcode   = kEplSuccessful
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvSendTxMsg              (tEdrvTxBuffer * pTxBuffer_p)
{
tEplKernel      Ret = kEplSuccessful;
unsigned int    uiBufferNumber;
tEdrvTxDesc*    pTxDesc;

    uiBufferNumber = pTxBuffer_p->m_BufferNumber.m_dwVal;

    if ((uiBufferNumber >= EDRV_MAX_TX_BUFFERS)
        || (EdrvInstance_l.m_afTxBufUsed[uiBufferNumber] == FALSE))
    {
        Ret = kEplEdrvBufNotExisting;
        goto Exit;
    }

    if (EdrvInstance_l.m_apTxBuffer[EdrvInstance_l.m_uiTailTxDesc] != NULL)
    {
        Ret = kEplEdrvNoFreeTxDesc;
        goto Exit;
    }

    EDRV_COUNT_SEND;

    // save pointer to buffer structure for TxHandler
    EdrvInstance_l.m_apTxBuffer[EdrvInstance_l.m_uiTailTxDesc] = pTxBuffer_p;

    pTxDesc = &EdrvInstance_l.m_pTxDesc[EdrvInstance_l.m_uiTailTxDesc];
    pTxDesc->m_le_qwBufferAddr = EdrvInstance_l.m_pTxBufDma + (uiBufferNumber * EDRV_MAX_FRAME_SIZE);
    pTxDesc->m_le_dwStatus = 0;
    pTxDesc->m_le_dwLengthCmd = ((DWORD) pTxBuffer_p->m_uiTxMsgLen) | EDRV_TX_DESC_CMD_DEF;

    // increment Tx descriptor queue tail pointer
    EdrvInstance_l.m_uiTailTxDesc++;
    if (EdrvInstance_l.m_uiTailTxDesc >= EDRV_MAX_TX_DESCS)
    {
        EdrvInstance_l.m_uiTailTxDesc = 0;
    }

    // start transmission
    EDRV_REGDW_WRITE(EDRV_REGDW_TDT, EdrvInstance_l.m_uiTailTxDesc);

Exit:
    return Ret;
}

#if 0
//---------------------------------------------------------------------------
//
// Function:    EdrvTxMsgReady
//
// Description: starts copying the buffer to the ethernet controller's FIFO
//
// Parameters:  pbBuffer_p - bufferdescriptor to transmit
//
// Returns:     Errorcode - kEplSuccessful
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvTxMsgReady              (tEdrvTxBuffer * pBuffer_p)
{
tEplKernel Ret = kEplSuccessful;
unsigned int uiBufferNumber;


Exit:
    return Ret;
}


//---------------------------------------------------------------------------
//
// Function:    EdrvTxMsgStart
//
// Description: starts transmission of the ethernet controller's FIFO
//
// Parameters:  pbBuffer_p - bufferdescriptor to transmit
//
// Returns:     Errorcode - kEplSuccessful
//
// State:
//
//---------------------------------------------------------------------------
tEplKernel EdrvTxMsgStart              (tEdrvTxBuffer * pBuffer_p)
{
tEplKernel Ret = kEplSuccessful;



    return Ret;
}
#endif



//---------------------------------------------------------------------------
//
// Function:    EdrvReinitRx
//
// Description: reinitialize the Rx process, because of error
//
// Parameters:  void
//
// Returns:     void
//
// State:
//
//---------------------------------------------------------------------------
#if 0
static void EdrvReinitRx(void)
{
}
#endif


int EdrvGetDiagnostics(char* pszBuffer_p, int iSize_p)
{
tEdrvTxDesc*    pTxDesc;
DWORD           dwTxStatus;
int             iUsedSize = 0;
int             iIndex;

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Head: %u (%lu)\n",
                       EdrvInstance_l.m_uiHeadTxDesc,
                       (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_TDH));

    pTxDesc = &EdrvInstance_l.m_pTxDesc[EdrvInstance_l.m_uiHeadTxDesc];
    dwTxStatus = pTxDesc->m_le_dwStatus;

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Headstatus: %lX\n", (unsigned long) dwTxStatus);

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Tail: %u (%lu)\n",
                       EdrvInstance_l.m_uiTailTxDesc,
                       (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_TDT));

    for (iIndex = 0; iIndex < 16; iIndex++)
    {
        iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                           "RAH[%2u] RAL[%2u]: 0x%08lX 0x%08lX\n",
                           iIndex, iIndex,
                           (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_RAH(iIndex)),
                           (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_RAL(iIndex)));
    }

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Interrupt Mask Set/Read Register:    0x%08lX\n",
                       (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_IMS));

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Receive Control Register:            0x%08lX\n",
                       (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_RCTL));

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Receive Descripter Control Register: 0x%08lX\n",
                       (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_RXDCTL));

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Receive Descripter Lenght Register:  0x%08lX\n",
                       (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_RDLEN0));

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Receive Descripter Head Register:    0x%08lX (%u)\n",
                       (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_RDH0),
                       EdrvInstance_l.m_uiHeadRxDesc);

    iUsedSize += snprintf (pszBuffer_p + iUsedSize, iSize_p - iUsedSize,
                       "Receive Descripter Tail Register:    0x%08lX (%u)\n",
                       (unsigned long) EDRV_REGDW_READ(EDRV_REGDW_RDT0),
                       EdrvInstance_l.m_uiTailRxDesc);

    return iUsedSize;
}


//---------------------------------------------------------------------------
//
// Function:     EdrvInterruptHandler
//
// Description:  interrupt handler
//
// Parameters:   void
//
// Returns:      void
//
// State:
//
//---------------------------------------------------------------------------
#if 0
void EdrvInterruptHandler (void)
{
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)
static irqreturn_t TgtEthIsr (int nIrqNum_p, void* ppDevInstData_p)
#else
static int TgtEthIsr (int nIrqNum_p, void* ppDevInstData_p, struct pt_regs* ptRegs_p)
#endif
{
tEdrvTxBuffer*  pTxBuffer;
tEdrvTxDesc*    pTxDesc;
DWORD           dwStatus;
DWORD           dwTxStatus;
int             iHandled = IRQ_HANDLED;
unsigned int    uiTxCount = 0;

//    EdrvInterruptHandler();

    // read the interrupt status
    dwStatus = EDRV_REGDW_READ(EDRV_REGDW_ICR);

    if ((dwStatus & EDRV_REGDW_INT_MASK_DEF) == 0)
    {
        iHandled = IRQ_NONE;
        EDRV_COUNT_PCI_ERR;
        goto Exit;
    }

    if ((dwStatus & EDRV_REGDW_INT_INT_ASSERTED) == 0)
    {   // acknowledge manually
        EDRV_REGDW_WRITE(EDRV_REGDW_ICR, dwStatus);
    }
        
    // process tasks
    if ((dwStatus & EDRV_REGDW_INT_TXDW) != 0)
    {   // transmit interrupt

        if (EdrvInstance_l.m_pbTxBuf == NULL)
        {
            printk("%s Tx buffers currently not allocated\n", __FUNCTION__);
            goto Exit;
        }

        do
        {
            pTxDesc = &EdrvInstance_l.m_pTxDesc[EdrvInstance_l.m_uiHeadTxDesc];
            // read transmit status
            dwTxStatus = pTxDesc->m_le_dwStatus;
            if ((dwTxStatus & EDRV_TX_DESC_STATUS_DD) != 0)
            {   // transmit finished
                // delete DD flag
                pTxDesc->m_le_dwStatus = 0;

                pTxBuffer = EdrvInstance_l.m_apTxBuffer[EdrvInstance_l.m_uiHeadTxDesc];
                EdrvInstance_l.m_apTxBuffer[EdrvInstance_l.m_uiHeadTxDesc] = NULL;

                uiTxCount++;

                // increment Tx descriptor queue head pointer
                EdrvInstance_l.m_uiHeadTxDesc++;
                if (EdrvInstance_l.m_uiHeadTxDesc >= EDRV_MAX_TX_DESCS)
                {
                    EdrvInstance_l.m_uiHeadTxDesc = 0;
                }

                if ((dwTxStatus & EDRV_TX_DESC_STATUS_EC) != 0)
                {
                    EDRV_COUNT_TX_COL_RL;
                }
                else if ((dwTxStatus & EDRV_TX_DESC_STATUS_LC) != 0)
                {
                    EDRV_COUNT_LATECOLLISION;
                }
                else
                {
                    EDRV_COUNT_TX;
                }

                if (pTxBuffer != NULL)
                {
                    // call Tx handler of Data link layer
                    if (pTxBuffer->m_pfnTxHandler != NULL)
                    {
                        pTxBuffer->m_pfnTxHandler(pTxBuffer);
                    }
                }
                else
                {
                    EDRV_COUNT_TX_FUN;
                }
            }
            else
            {
                if (uiTxCount == 0)
                {
                    EDRV_COUNT_TX_ERR;
                }
                break;
            }
        }
        while (EdrvInstance_l.m_uiHeadTxDesc != EdrvInstance_l.m_uiTailTxDesc);
    }

    if ((dwStatus & (EDRV_REGDW_INT_RXT0 | EDRV_REGDW_INT_SRPD | EDRV_REGDW_INT_RXDMT0)) != 0)
    {   // receive interrupt
    tEdrvRxDesc*    pRxDesc;
    tEdrvRxBuffer   RxBuffer;
    unsigned int    uiLength;
    BYTE            bRxStatus;
    BYTE            bRxError;

        EDRV_COUNT_RX_ALL;

        if (EdrvInstance_l.m_pbRxBuf == NULL)
        {
            printk("%s Rx buffers currently not allocated\n", __FUNCTION__);
            goto Exit;
        }

        pRxDesc = &EdrvInstance_l.m_pRxDesc[EdrvInstance_l.m_uiHeadRxDesc];

        while (pRxDesc->m_bStatus != 0)
        {   // frame available

            bRxStatus = pRxDesc->m_bStatus;
            bRxError  = pRxDesc->m_bError;

            if ((bRxStatus & EDRV_RXSTAT_DD) != 0)
            {   // descriptor is valid

                if ((bRxStatus & EDRV_RXSTAT_EOP) == 0)
                {   // multiple descriptors used for one packet
                    EDRV_COUNT_RX_ERR;
                }
                else if ((bRxError & EDRV_RXERR_CE) != 0)
                {   // CRC error
                    EDRV_COUNT_RX_CRC;
                }
                else if ((bRxError & EDRV_RXERR_SEQ) != 0)
                {   // packet sequence error
                    EDRV_COUNT_RX_SEQ;
                }
                else
                {   // packet is OK
                    RxBuffer.m_BufferInFrame = kEdrvBufferLastInFrame;

                    // get length of received packet
                    // m_le_wLength does not contain CRC as EDRV_REGDW_RCTL_SECRC is set
                    uiLength = AmiGetWordFromLe(&pRxDesc->m_le_wLength);
                    RxBuffer.m_uiRxMsgLen = uiLength;

                    // calculate pointer to current packet in receive buffer
                    RxBuffer.m_pbBuffer = EdrvInstance_l.m_pbRxBuf + EDRV_RX_BUFFER_PER_DESC_SIZE * EdrvInstance_l.m_uiHeadRxDesc;

                    EDRV_COUNT_RX;

                    // call Rx handler of Data link layer
                    EdrvInstance_l.m_InitParam.m_pfnRxHandler(&RxBuffer);
                }
            }
            else
            {   // status written by hardware but desc not done
                EDRV_COUNT_RX_ERR;
            }

//            printk("Status: 0x%02x ", (unsigned int)pRxDesc->m_bStatus);
            pRxDesc->m_bStatus = 0;

            EdrvInstance_l.m_uiHeadRxDesc++;
            if (EdrvInstance_l.m_uiHeadRxDesc >= EDRV_MAX_RX_DESCS)
            {
                EdrvInstance_l.m_uiHeadRxDesc = 0;
            }
        }

        // release receive descriptors
        if (EdrvInstance_l.m_uiHeadRxDesc == 0)
        {
            EdrvInstance_l.m_uiTailRxDesc = EDRV_MAX_RX_DESCS - 1;
        }
        else
        {
            EdrvInstance_l.m_uiTailRxDesc = EdrvInstance_l.m_uiHeadRxDesc - 1;
        }

        EDRV_REGDW_WRITE(EDRV_REGDW_RDT0, EdrvInstance_l.m_uiTailRxDesc);
    }

    if ((dwStatus & (EDRV_REGDW_INT_RXSEQ | EDRV_REGDW_INT_RXO)) != 0)
    {   // receive error interrupt

        if ((dwStatus & (EDRV_REGDW_INT_RXSEQ)) != 0)
        {   // Ethernet frame sequencing error
            EDRV_COUNT_RX_SEQ;
        }

        if ((dwStatus & (EDRV_REGDW_INT_RXO)) != 0)
        {   // receive queue overrun
            EDRV_COUNT_RX_ORUN;
        }
    }

Exit:
    return iHandled;
}


//---------------------------------------------------------------------------
//
// Function:    EdrvInitOne
//
// Description: initializes one PCI device
//
// Parameters:  pPciDev             = pointer to corresponding PCI device structure
//              pId                 = PCI device ID
//
// Returns:     (int)               = error code
//
// State:
//
//---------------------------------------------------------------------------

static int EdrvInitOne(struct pci_dev *pPciDev,
                       const struct pci_device_id *pId)
{
int     iResult = 0;
DWORD   dwTemp;
QWORD   qwDescAddress;
int     iIndex;

    if (EdrvInstance_l.m_pPciDev != NULL)
    {   // Edrv is already connected to a PCI device
        printk("%s device %s discarded\n", __FUNCTION__, pci_name(pPciDev));
        iResult = -ENODEV;
        goto Exit;
    }

    // enable device
    printk("%s enable device\n", __FUNCTION__);
    iResult = pci_enable_device(pPciDev);
    if (iResult != 0)
    {
        goto Exit;
    }

    EdrvInstance_l.m_pPciDev = pPciDev;

    if (EdrvInstance_l.m_pPciDev == NULL)
    {
        printk("%s pPciDev==NULL\n", __FUNCTION__);
    }

    printk("%s request regions\n", __FUNCTION__);
    iResult = pci_request_regions(pPciDev, DRV_NAME);
    if (iResult != 0)
    {
        goto ExitFail;
    }

    printk("%s ioremap\n", __FUNCTION__);
    EdrvInstance_l.m_pIoAddr = ioremap (pci_resource_start(pPciDev, 0), pci_resource_len(pPciDev, 0));
    if (EdrvInstance_l.m_pIoAddr == NULL)
    {   // remap of controller's register space failed
        iResult = -EIO;
        goto ExitFail;
    }

    // enable PCI busmaster
    printk("%s enable busmaster\n", __FUNCTION__);
    pci_set_master (pPciDev);

    // disable GIO Master accesses
    dwTemp = EDRV_REGDW_READ(EDRV_REGDW_CTRL);
    dwTemp |= EDRV_REGDW_CTRL_MST_DIS;
    EDRV_REGDW_WRITE(EDRV_REGDW_CTRL, dwTemp);

    // wait until master is disabled
    for (iIndex = EDRV_MASTER_DISABLE_TIMEOUT; iIndex > 0; iIndex--)
    {
        if ((EDRV_REGDW_READ(EDRV_REGDW_STATUS) & EDRV_REGDW_STATUS_MST_EN) == 0)
        {
            break;
        }

        msleep(1);
    }
    if (iIndex == 0)
    {
        iResult = -EIO;
        goto ExitFail;
    }

    // disable interrupts
    printk("%s disable interrupts\n", __FUNCTION__);
    EDRV_REGDW_WRITE(EDRV_REGDW_IMC, EDRV_REGDW_INT_MASK_ALL);

    // reset controller
    printk("%s reset controller\n", __FUNCTION__);
    dwTemp |= EDRV_REGDW_CTRL_RST;
    EDRV_REGDW_WRITE(EDRV_REGDW_CTRL, dwTemp);

    // wait until reset has finished and configuration from EEPROM was read
    for (iIndex = EDRV_AUTO_READ_DONE_TIMEOUT; iIndex > 0; iIndex--)
    {
        if ((EDRV_REGDW_READ(EDRV_REGDW_EEC) & EDRV_REGDW_EEC_AUTO_RD) != 0)
        {
            break;
        }

        msleep(1);
    }
    if (iIndex == 0)
    {
        iResult = -EIO;
        goto ExitFail;
    }

    // disable interrupts
    printk("%s disable interrupts\n", __FUNCTION__);
    EDRV_REGDW_WRITE(EDRV_REGDW_IMC, EDRV_REGDW_INT_MASK_ALL);
    dwTemp = EDRV_REGDW_READ(EDRV_REGDW_ICR);

    // set global configuration
    printk("%s set global configuration\n", __FUNCTION__);
    EDRV_REGDW_WRITE(EDRV_REGDW_CTRL, EDRV_REGDW_CTRL_DEF);

    // PHY reset by software:
    //1. Obtain the Software/Firmware semaphore (SWSM.SWESMBI - 05B50h; bit 1). Set it to 1b.
    //2. Drive PHY reset (CTRL.PHY_RST at offset 0000h [bit 31], write 1b, wait 100 us, and then
    //write 0b).
    //3. Delay 10 ms
    //4. Start configuring the PHY.
    //5. Release the Software/Firmware semaphore

    // set TIPG
    printk("%s set TIPG\n", __FUNCTION__);
    EDRV_REGDW_WRITE(EDRV_REGDW_TIPG, EDRV_REGDW_TIPG_DEF);

    // install interrupt handler
    printk("%s install interrupt handler\n", __FUNCTION__);
    iResult = request_irq(pPciDev->irq, TgtEthIsr, IRQF_SHARED, DRV_NAME, pPciDev);
    if (iResult != 0)
    {
        goto ExitFail;
    }

    // allocate buffers
    printk("%s allocate buffers\n", __FUNCTION__);
    EdrvInstance_l.m_pbTxBuf = pci_alloc_consistent(pPciDev, EDRV_TX_BUFFER_SIZE,
                     &EdrvInstance_l.m_pTxBufDma);
    if (EdrvInstance_l.m_pbTxBuf == NULL)
    {
        iResult = -ENOMEM;
        goto ExitFail;
    }

    EdrvInstance_l.m_pTxDesc = pci_alloc_consistent(pPciDev, EDRV_TX_DESCS_SIZE,
                     &EdrvInstance_l.m_pTxDescDma);
    if (EdrvInstance_l.m_pTxDesc == NULL)
    {
        iResult = -ENOMEM;
        goto ExitFail;
    }

    EdrvInstance_l.m_pbRxBuf = pci_alloc_consistent(pPciDev, EDRV_RX_BUFFER_SIZE,
                     &EdrvInstance_l.m_pRxBufDma);
    if (EdrvInstance_l.m_pbRxBuf == NULL)
    {
        iResult = -ENOMEM;
        goto ExitFail;
    }

    EdrvInstance_l.m_pRxDesc = pci_alloc_consistent(pPciDev, EDRV_RX_DESCS_SIZE,
                     &EdrvInstance_l.m_pRxDescDma);
    if (EdrvInstance_l.m_pRxDesc == NULL)
    {
        iResult = -ENOMEM;
        goto ExitFail;
    }

    // check if user specified a MAC address
    printk("%s check specified MAC address\n", __FUNCTION__);
    if ((EdrvInstance_l.m_InitParam.m_abMyMacAddr[0] != 0) |
        (EdrvInstance_l.m_InitParam.m_abMyMacAddr[1] != 0) |
        (EdrvInstance_l.m_InitParam.m_abMyMacAddr[2] != 0) |
        (EdrvInstance_l.m_InitParam.m_abMyMacAddr[3] != 0) |
        (EdrvInstance_l.m_InitParam.m_abMyMacAddr[4] != 0) |
        (EdrvInstance_l.m_InitParam.m_abMyMacAddr[5] != 0)  )
    {   // write specified MAC address to controller
        dwTemp = 0;
        EDRV_REGDW_WRITE(EDRV_REGDW_RAH(0), dwTemp); // disable Entry
        dwTemp |= EdrvInstance_l.m_InitParam.m_abMyMacAddr[0] <<  0;
        dwTemp |= EdrvInstance_l.m_InitParam.m_abMyMacAddr[1] <<  8;
        dwTemp |= EdrvInstance_l.m_InitParam.m_abMyMacAddr[2] << 16;
        dwTemp |= EdrvInstance_l.m_InitParam.m_abMyMacAddr[3] << 24;
        EDRV_REGDW_WRITE(EDRV_REGDW_RAL(0), dwTemp);
        dwTemp = 0;
        dwTemp |= EdrvInstance_l.m_InitParam.m_abMyMacAddr[4] <<  0;
        dwTemp |= EdrvInstance_l.m_InitParam.m_abMyMacAddr[5] <<  8;
        dwTemp |= EDRV_REGDW_RAH_AV;
        EDRV_REGDW_WRITE(EDRV_REGDW_RAH(0), dwTemp);
    }
    else
    {   // read MAC address from controller
        dwTemp = EDRV_REGDW_READ(EDRV_REGDW_RAL(0));
        EdrvInstance_l.m_InitParam.m_abMyMacAddr[0] = (dwTemp >>  0) & 0xFF;
        EdrvInstance_l.m_InitParam.m_abMyMacAddr[1] = (dwTemp >>  8) & 0xFF;
        EdrvInstance_l.m_InitParam.m_abMyMacAddr[2] = (dwTemp >> 16) & 0xFF;
        EdrvInstance_l.m_InitParam.m_abMyMacAddr[3] = (dwTemp >> 24) & 0xFF;
        dwTemp = EDRV_REGDW_READ(EDRV_REGDW_RAH(0));
        EdrvInstance_l.m_InitParam.m_abMyMacAddr[4] = (dwTemp >>  0) & 0xFF;
        EdrvInstance_l.m_InitParam.m_abMyMacAddr[5] = (dwTemp >>  8) & 0xFF;
    }

    // initialize Multicast Table Array to 0
    for (iIndex = 0; iIndex < 128; iIndex++)
    {
        EDRV_REGDW_WRITE(EDRV_REGDW_MTA(iIndex), 0);
    }

    // initialize Rx descriptors
    printk("%s initialize Rx descriptors\n", __FUNCTION__);
    for (iIndex = 0; iIndex < EDRV_MAX_RX_DESCS; iIndex++)
    {
        EdrvInstance_l.m_pRxDesc[iIndex].m_le_qwBufferAddr = EdrvInstance_l.m_pRxBufDma + (iIndex * EDRV_RX_BUFFER_PER_DESC_SIZE);
        EdrvInstance_l.m_pRxDesc[iIndex].m_bStatus         = 0;
    }
    EDRV_REGDW_WRITE(EDRV_REGDW_RXDCTL, EDRV_REGDW_RXDCTL_DEF);
    // Rx buffer size is set to 2048 by default
    // Rx descriptor typ is set to legacy by default
    qwDescAddress = EdrvInstance_l.m_pRxDescDma;
    EDRV_REGDW_WRITE(EDRV_REGDW_RDBAL0, (qwDescAddress & 0xFFFFFFFF));
    EDRV_REGDW_WRITE(EDRV_REGDW_RDBAH0, (qwDescAddress >> 32));
    EDRV_REGDW_WRITE(EDRV_REGDW_RDLEN0, EDRV_RX_DESCS_SIZE);
    EdrvInstance_l.m_uiHeadRxDesc = 0;
    EDRV_REGDW_WRITE(EDRV_REGDW_RDH0, 0);
    EdrvInstance_l.m_uiTailRxDesc = EDRV_MAX_RX_DESCS - 1;
    EDRV_REGDW_WRITE(EDRV_REGDW_RDT0, EDRV_MAX_RX_DESCS - 1);

    // enable receiver
    printk("%s set Rx conf register\n", __FUNCTION__);
    EDRV_REGDW_WRITE(EDRV_REGDW_RCTL, EDRV_REGDW_RCTL_DEF);

    // initialize Tx descriptors
    printk("%s initialize Tx descriptors\n", __FUNCTION__);
    EPL_MEMSET(EdrvInstance_l.m_apTxBuffer, 0, sizeof (EdrvInstance_l.m_apTxBuffer));
    EDRV_REGDW_WRITE(EDRV_REGDW_TXDCTL, EDRV_REGDW_TXDCTL_DEF);
    qwDescAddress = EdrvInstance_l.m_pTxDescDma;
    EDRV_REGDW_WRITE(EDRV_REGDW_TDBAL, (qwDescAddress & 0xFFFFFFFF));
    EDRV_REGDW_WRITE(EDRV_REGDW_TDBAH, (qwDescAddress >> 32));
    EDRV_REGDW_WRITE(EDRV_REGDW_TDLEN, EDRV_TX_DESCS_SIZE);
    EDRV_REGDW_WRITE(EDRV_REGDW_TDH, 0);
    EDRV_REGDW_WRITE(EDRV_REGDW_TDT, 0);

    // enable transmitter
    printk("%s set Tx conf register\n", __FUNCTION__);
    EDRV_REGDW_WRITE(EDRV_REGDW_TCTL, EDRV_REGDW_TCTL_DEF);

/*
    // clear missed packet counter to enable Rx/Tx process
    EDRV_REGDW_WRITE(EDRV_REGDW_MPC, 0);
*/

    // enable interrupts
    printk("%s enable interrupts\n", __FUNCTION__);
    EDRV_REGDW_WRITE(EDRV_REGDW_IMS, EDRV_REGDW_INT_MASK_DEF);

    goto Exit;

ExitFail:
    EdrvRemoveOne(pPciDev);

Exit:
    printk("%s finished with %d\n", __FUNCTION__, iResult);
    return iResult;
}


//---------------------------------------------------------------------------
//
// Function:    EdrvRemoveOne
//
// Description: shuts down one PCI device
//
// Parameters:  pPciDev             = pointer to corresponding PCI device structure
//
// Returns:     (void)
//
// State:
//
//---------------------------------------------------------------------------

static void EdrvRemoveOne(struct pci_dev *pPciDev)
{
DWORD   dwTemp;

    if (EdrvInstance_l.m_pPciDev != pPciDev)
    {   // trying to remove unknown device
        BUG_ON(EdrvInstance_l.m_pPciDev != pPciDev);
        goto Exit;
    }

    if (EdrvInstance_l.m_pIoAddr != NULL)
    {
        // disable transmitter and receiver
        EDRV_REGDW_WRITE(EDRV_REGDW_TCTL, 0);

        // disable interrupts
        EDRV_REGDW_WRITE(EDRV_REGDW_IMC, EDRV_REGDW_INT_MASK_ALL);
        dwTemp = EDRV_REGDW_READ(EDRV_REGDW_ICR);
    }

    // remove interrupt handler
    free_irq(pPciDev->irq, pPciDev);


    // free buffers
    if (EdrvInstance_l.m_pbTxBuf != NULL)
    {
        pci_free_consistent(pPciDev, EDRV_TX_BUFFER_SIZE,
                     EdrvInstance_l.m_pbTxBuf, EdrvInstance_l.m_pTxBufDma);
        EdrvInstance_l.m_pbTxBuf = NULL;
    }

    if (EdrvInstance_l.m_pTxDesc != NULL)
    {
        pci_free_consistent(pPciDev, EDRV_TX_DESCS_SIZE,
                     EdrvInstance_l.m_pTxDesc, EdrvInstance_l.m_pTxDescDma);
        EdrvInstance_l.m_pTxDesc = NULL;
    }

    if (EdrvInstance_l.m_pbRxBuf != NULL)
    {
        pci_free_consistent(pPciDev, EDRV_RX_BUFFER_SIZE,
                     EdrvInstance_l.m_pbRxBuf, EdrvInstance_l.m_pRxBufDma);
        EdrvInstance_l.m_pbRxBuf = NULL;
    }

    // unmap controller's register space
    if (EdrvInstance_l.m_pIoAddr != NULL)
    {
        iounmap(EdrvInstance_l.m_pIoAddr);
        EdrvInstance_l.m_pIoAddr = NULL;
    }

    // disable the PCI device
    pci_disable_device(pPciDev);

    // release memory regions
    pci_release_regions(pPciDev);

    EdrvInstance_l.m_pPciDev = NULL;

Exit:;
}


//---------------------------------------------------------------------------
//
// Function:    EdrvCalcHash
//
// Description: function calculates the entry for the hash-table from MAC
//              address
//
//
// Parameters:  pbMAC_p - pointer to MAC address
//
// Returns:     hash value
//
// State:       at the moment, only perfect filters are used
//
//---------------------------------------------------------------------------

#if 0
static BYTE EdrvCalcHash (BYTE * pbMAC_p)
{
}
#endif

