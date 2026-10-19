/*
 * Linked into every PrismNet program (not the libraries): libnix's stdio
 * buffers default to 64 KB each, allocated at start-up for stdout and
 * stderr. Each needs one contiguous 64 KB block, which a 1-2 MB chip-RAM
 * machine often no longer has after the program itself is loaded, and
 * libnix then exits with return code 20 without a word. 2 KB is plenty for
 * command-line output.
 * GPL-3.0-only, see LICENSE.
 */
static unsigned long pn_bufsiz = 2048;
unsigned long *__BUFSIZE = &pn_bufsiz;
