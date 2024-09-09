/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2012  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * Copyright (C) 2014 Fredrik Ahlberg <fredrik@z80.se>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements capture of the TRACESWO output.
 *
 * ARM DDI 0403D - ARMv7M Architecture Reference Manual
 * ARM DDI 0337I - Cortex-M3 Technical Reference Manual
 * ARM DDI 0314H - CoreSight Components Technical Reference Manual
 */

#include "general.h"
#include "platform.h"
#include "usb.h"

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/lm4f/rcc.h>
#include <libopencm3/lm4f/nvic.h>
#include <libopencm3/lm4f/uart.h>
#include <libopencm3/usb/usbd.h>

void swo_uart_init(void)
{
	periph_clock_enable(RCC_GPIOD);
	periph_clock_enable(SWO_UART_CLK);
	__asm__("nop");
	__asm__("nop");
	__asm__("nop");

	gpio_mode_setup(SWO_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, SWO_PIN);
	gpio_set_af(SWO_PORT, 1, SWO_PIN); /* U2RX */

	uart_disable(SWO_UART);

	/* Setup UART parameters. */
	uart_clock_from_sysclk(SWO_UART);
	uart_set_baudrate(SWO_UART, 800000);
	uart_set_databits(SWO_UART, 8);
	uart_set_stopbits(SWO_UART, 1);
	uart_set_parity(SWO_UART, UART_PARITY_NONE);

	// Enable FIFO
	uart_enable_fifo(SWO_UART);

	// Set FIFO interrupt trigger levels to 4/8 full for RX buffer and
	// 7/8 empty (1/8 full) for TX buffer
	uart_set_fifo_trigger_levels(SWO_UART, UART_FIFO_RX_TRIG_1_2, UART_FIFO_TX_TRIG_7_8);

	uart_clear_interrupt_flag(SWO_UART, UART_INT_RX | UART_INT_RT);

	/* Enable interrupts */
	uart_enable_interrupts(SWO_UART, UART_INT_RX | UART_INT_RT);

	/* Finally enable the USART. */
	uart_enable(SWO_UART);

	nvic_set_priority(SWO_UART_IRQ, 0);
	nvic_enable_irq(SWO_UART_IRQ);

	/* Un-stall USB endpoint */
	usbd_ep_stall_set(usbdev, USB_REQ_TYPE_IN | SWO_ENDPOINT, 0);

	gpio_mode_setup(GPIOD, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO3);
}

void traceswo_baud(unsigned int baud)
{
	uart_set_baudrate(SWO_UART, baud);
	uart_set_databits(SWO_UART, 8);
}

uint32_t swo_uart_get_baudrate(void)
{
	return uart_get_baudrate(SWO_UART);
}

#define FIFO_SIZE 256U

/* RX Fifo buffer */
static volatile uint8_t buf_rx[FIFO_SIZE];
/* Fifo in pointer, writes assumed to be atomic, should be only incremented within RX ISR */
static volatile uint32_t buf_rx_in = 0;
/* Fifo out pointer, writes assumed to be atomic, should be only incremented outside RX ISR */
static volatile uint32_t buf_rx_out = 0;

void trace_buf_push(void)
{
	size_t len;

	if (buf_rx_in == buf_rx_out)
		return;

	if (buf_rx_in > buf_rx_out)
		len = buf_rx_in - buf_rx_out;
	else
		len = FIFO_SIZE - buf_rx_out;

	if (len > 64U)
		len = 64;

	if (usbd_ep_write_packet(usbdev, USB_REQ_TYPE_IN | SWO_ENDPOINT, (uint8_t *)&buf_rx[buf_rx_out], len) == len) {
		buf_rx_out += len;
		buf_rx_out %= FIFO_SIZE;
	}
}

void swo_send_buffer(usbd_device *dev, uint8_t ep)
{
	(void)dev;
	(void)ep;
	trace_buf_push();
}

void trace_tick(void)
{
	trace_buf_push();
}

void SWO_UART_ISR(void)
{
	uint32_t flush = uart_is_interrupt_source(SWO_UART, UART_INT_RT);

	while (!uart_is_rx_fifo_empty(SWO_UART)) {
		const uint32_t c = uart_recv(SWO_UART);

		/* If the next increment of rx_in would put it at the same point
		* as rx_out, the FIFO is considered full.
		*/
		if ((buf_rx_in + 1U) % FIFO_SIZE != buf_rx_out) {
			/* insert into FIFO */
			buf_rx[buf_rx_in++] = c;

			/* wrap out pointer */
			if (buf_rx_in >= FIFO_SIZE)
				buf_rx_in = 0;
		} else {
			flush = 1;
			break;
		}
	}

	if (flush)
		/* advance fifo out pointer by amount written */
		trace_buf_push();
}
