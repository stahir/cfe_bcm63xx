/*  *********************************************************************
    *  Broadcom Common Firmware Environment (CFE)
    *  
    *  NS16550 UART driver			File: dev_ns16550.c
    *  
    *  This is a console device driver for an NS16550 UART, either
    *  on-board or as a PCI-device.  In the case of a PCI device,
    *  our probe routine is called from the PCI probe code
    *  over in dev_ns16550_pci.c
    *  
    *  Author:  Mitch Lichtenberg (mpl@broadcom.com)
    *  
    *********************************************************************  
    *
    *  Copyright 2000,2001,2002,2003
    *  Broadcom Corporation. All rights reserved.
    *  
    *  This software is furnished under license and may be used and 
    *  copied only in accordance with the following terms and 
    *  conditions.  Subject to these conditions, you may download, 
    *  copy, install, use, modify and distribute modified or unmodified 
    *  copies of this software in source and/or binary form.  No title 
    *  or ownership is transferred hereby.
    *  
    *  1) Any source code used, modified or distributed must reproduce 
    *     and retain this copyright notice and list of conditions 
    *     as they appear in the source file.
    *  
    *  2) No right is granted to use any trade name, trademark, or 
    *     logo of Broadcom Corporation.  The "Broadcom Corporation" 
    *     name may not be used to endorse or promote products derived 
    *     from this software without the prior written permission of 
    *     Broadcom Corporation.
    *  
    *  3) THIS SOFTWARE IS PROVIDED "AS-IS" AND ANY EXPRESS OR
    *     IMPLIED WARRANTIES, INCLUDING BUT NOT LIMITED TO, ANY IMPLIED
    *     WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR 
    *     PURPOSE, OR NON-INFRINGEMENT ARE DISCLAIMED. IN NO EVENT 
    *     SHALL BROADCOM BE LIABLE FOR ANY DAMAGES WHATSOEVER, AND IN 
    *     PARTICULAR, BROADCOM SHALL NOT BE LIABLE FOR DIRECT, INDIRECT,
    *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
    *     (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
    *     GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
    *     BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY 
    *     OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR 
    *     TORT (INCLUDING NEGLIGENCE OR OTHERWISE), EVEN IF ADVISED OF 
    *     THE POSSIBILITY OF SUCH DAMAGE.
    ********************************************************************* */


#include "lib_types.h"
#include "lib_malloc.h"
#include "lib_printf.h"
#include "cfe_iocb.h"
#include "cfe_device.h"
#include "cfe_ioctl.h"

#include "lib_physio.h"

#include "bsp_config.h"

#include "ns16550.h"

#define WRITECSR(p,v) phys_write8((p),(v))
#define READCSR(p) phys_read8((p))

static int ns16550_uart_open(cfe_devctx_t *ctx);
static int ns16550_uart_read(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int ns16550_uart_inpstat(cfe_devctx_t *ctx,iocb_inpstat_t *inpstat);
static int ns16550_uart_write(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int ns16550_uart_ioctl(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int ns16550_uart_close(cfe_devctx_t *ctx);

void ns16550_uart_probe(cfe_driver_t *drv,
			unsigned long probe_a, unsigned long probe_b, 
			void *probe_ptr);


const cfe_devdisp_t ns16550_uart_dispatch = {
    ns16550_uart_open,
    ns16550_uart_read,
    ns16550_uart_inpstat,
    ns16550_uart_write,
    ns16550_uart_ioctl,
    ns16550_uart_close,	
    NULL,
    NULL
};

const cfe_driver_t ns16550_uart = {
    "NS16550 UART",
    "uart",
    CFE_DEV_SERIAL,
    &ns16550_uart_dispatch,
    ns16550_uart_probe
};

typedef struct ns16550_uart_s {
    physaddr_t uart_base;
    int uart_flowcontrol;
    int uart_speed;
} ns16550_uart_t;


/* 
 * NS16550-compatible UART.
 * probe_a: physical address of UART
 */

void ns16550_uart_probe(cfe_driver_t *drv,
			unsigned long probe_a, unsigned long probe_b, 
			void *probe_ptr)
{
    ns16550_uart_t *softc;
    char descr[80];

    softc = (ns16550_uart_t *) KMALLOC(sizeof(ns16550_uart_t),0);
    if (softc) {
	softc->uart_base = probe_a;
	softc->uart_speed = CFG_SERIAL_BAUD_RATE;
	softc->uart_flowcontrol = SERIAL_FLOW_NONE;
	xsprintf(descr, "%s at 0x%X", drv->drv_description, (uint32_t)probe_a);

	cfe_attach(drv, softc, NULL, descr);
	}
}

#define DELAY(n) delay(n)
extern int32_t _getticks(void);
static void delay(int ticks)
{
    int32_t t;

    t = _getticks() + ticks;
    while (_getticks() < t)
	; /* NULL LOOP */
}

static void ns16550_uart_setflow(ns16550_uart_t *softc)
{
    /* noop for now */
}


static int ns16550_uart_open(cfe_devctx_t *ctx)
{
    ns16550_uart_t *softc = ctx->dev_softc;
    int baudrate = CFG_SERIAL_BAUD_RATE;
    unsigned int brtc;

    brtc = BRTC(baudrate);

    WRITECSR(softc->uart_base+R_UART_CFCR,CFCR_DLAB);
    WRITECSR(softc->uart_base+R_UART_DATA,brtc & 0xFF);
    WRITECSR(softc->uart_base+R_UART_IER,brtc>>8);
    WRITECSR(softc->uart_base+R_UART_CFCR,CFCR_8BITS);
    WRITECSR(softc->uart_base+R_UART_MCR,MCR_DTR | MCR_RTS | MCR_IENABLE);
    WRITECSR(softc->uart_base+R_UART_IER,0);

    WRITECSR(softc->uart_base+R_UART_FIFO,FIFO_ENABLE);
    DELAY(100);
    WRITECSR(softc->uart_base+R_UART_FIFO,
	     FIFO_ENABLE | FIFO_RCV_RST | FIFO_XMT_RST | FIFO_TRIGGER_1);
    DELAY(100);

    if ((READCSR(softc->uart_base+R_UART_IIR) & IIR_FIFO_MASK) != IIR_FIFO_MASK) {
	WRITECSR(softc->uart_base+R_UART_FIFO,0);
	}
    ns16550_uart_setflow(softc);

    return 0;
}

static int ns16550_uart_read(cfe_devctx_t *ctx, iocb_buffer_t *buffer)
{
    ns16550_uart_t *softc = ctx->dev_softc;
    unsigned char *bptr;
    int blen;

    bptr = buffer->buf_ptr;
    blen = buffer->buf_length;

    while ((blen > 0) && (READCSR(softc->uart_base+R_UART_LSR) & LSR_RXRDY)) {
	*bptr++ = (READCSR(softc->uart_base+R_UART_DATA) & 0xFF);
	blen--;
	}

    buffer->buf_retlen = buffer->buf_length - blen;
    return 0;
}

static int ns16550_uart_inpstat(cfe_devctx_t *ctx, iocb_inpstat_t *inpstat)
{
    ns16550_uart_t *softc = ctx->dev_softc;

    inpstat->inp_status = (READCSR(softc->uart_base+R_UART_LSR) & LSR_RXRDY) ? 1 : 0;

    return 0;
}

static int ns16550_uart_write(cfe_devctx_t *ctx, iocb_buffer_t *buffer)
{
    ns16550_uart_t *softc = ctx->dev_softc;
    unsigned char *bptr;
    int blen;

    bptr = buffer->buf_ptr;
    blen = buffer->buf_length;
    while ((blen > 0) && (READCSR(softc->uart_base+R_UART_LSR) & LSR_TXRDY)) {
	WRITECSR(softc->uart_base+R_UART_DATA, *bptr++);
	blen--;
	}

    buffer->buf_retlen = buffer->buf_length - blen;
    return 0;
}

static int ns16550_uart_ioctl(cfe_devctx_t *ctx, iocb_buffer_t *buffer) 
{
    ns16550_uart_t *softc = ctx->dev_softc;

    unsigned int *info = (unsigned int *) buffer->buf_ptr;

    switch ((int)buffer->buf_ioctlcmd) {
	case IOCTL_SERIAL_GETSPEED:
	    *info = softc->uart_speed;
	    break;
	case IOCTL_SERIAL_SETSPEED:
	    softc->uart_speed = *info;
	    /* NYI */
	    break;
	case IOCTL_SERIAL_GETFLOW:
	    *info = softc->uart_flowcontrol;
	    break;
	case IOCTL_SERIAL_SETFLOW:
	    softc->uart_flowcontrol = *info;
	    ns16550_uart_setflow(softc);
	    break;
	default:
	    return -1;
	}

    return 0;
}

static int ns16550_uart_close(cfe_devctx_t *ctx)
{
    ns16550_uart_t *softc = ctx->dev_softc;

    WRITECSR(softc->uart_base+R_UART_MCR,0);

    return 0;
}


