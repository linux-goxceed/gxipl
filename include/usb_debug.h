/* SPDX-License-Identifier: MIT */
#ifndef USB_DEBUG_H
#define USB_DEBUG_H

/*
 * USB_DEBUG gates the USB bring-up diagnostics (register dumps, BOT/EHCI
 * error traces).  It is a build-time switch, not a runtime one, on purpose:
 * these strings and their call sites are worth ~400-530 B of .rodata plus
 * ~123 lines of text, and a runtime flag means every build pays for them
 * even when it can never print.
 *
 * Build with -DUSB_DEBUG=1 to restore the traces for a bring-up session.
 * The default 0 keeps release images silent, and lets LTO drop the string
 * literals entirely.
 *
 * This header is shared by the main stage-2 core (bootcode/usb/usb_msc.c) and
 * the minimal IPL core (ipl/usb/usb_msc_min.c) so both variants honour the
 * same switch and a non-min variant inherits the opt-in behaviour.
 *
 * Distinct from VERBOSE_MINIFY, which is the Makefile's switch for shortening
 * the *other* log strings; it does not cover the USB core.
 */
/*
 * NOTE: because this header always leaves USB_DEBUG *defined* (as 0), every
 * use site must test it with `#if USB_DEBUG`, never `#ifdef USB_DEBUG`.
 * A plain `#ifdef` would be true even in the default release build.
 */
#ifndef USB_DEBUG
#define USB_DEBUG	0
#endif

#endif
