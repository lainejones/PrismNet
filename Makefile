# PrismNet - build with bebbo amiga-gcc under WSL:
#   wsl -e bash -lc 'cd /mnt/c/projects/PrismNet && PATH=/opt/amiga/bin:$PATH make'
# Targets: all (default), clean.  DEBUG=1 enables lwIP debug output.

CC      = m68k-amigaos-gcc
AR      = m68k-amigaos-ar

CPU    ?= -m68000
# The core (lwIP + port + core/) is built for speed, everything else for size.
COREOPT ?= -O2 -DPN_CORE
CFLAGS  = $(CPU) -Os -noixemul -fomit-frame-pointer -Wall -Wno-pointer-sign \
          -Iport/include -Ilwip/src/include
LDFLAGS = $(CPU) -noixemul
ifeq ($(DEBUG),1)
CFLAGS += -DPRISMNET_DEBUG
OBJ     = build/objdebug
OUT     = out/debug
endif
ifeq ($(PROFILE),1)
CFLAGS += -DPN_PROFILE
OBJ     = build/objprof
OUT     = out/prof
endif

OBJ    ?= build/obj
OUT    ?= out

LWIP_SRC = $(wildcard lwip/src/core/*.c) $(wildcard lwip/src/core/ipv4/*.c) \
           lwip/src/netif/ethernet.c lwip/src/apps/netbiosns/netbiosns.c \
           lwip/src/apps/sntp/sntp.c \
           $(wildcard lwip/src/apps/mdns/*.c)
PORT_SRC = port/pnsys.c port/sanaif.c port/serslip.c port/pnclient.c port/pnmem.c port/chksum.S

LWIP_OBJ = $(patsubst lwip/src/%.c,$(OBJ)/lwip/%.o,$(LWIP_SRC))
# libnix's 64 KB stdio buffers stop programs starting on small machines
STDIO_OBJ = $(OBJ)/port/pnstdio.o
PORT_OBJ = $(patsubst port/%.S,$(OBJ)/port/%.o,$(patsubst port/%.c,$(OBJ)/port/%.o,$(PORT_SRC)))

CORE_SRC = core/core.c core/csock.c core/resolv.c core/config.c core/netstats.c
CORE_OBJ = $(patsubst core/%.c,$(OBJ)/core/%.o,$(CORE_SRC))

all: nettools servers nfs smb $(OUT)/PrismNet $(OUT)/bsdsocket.library $(OUT)/usergroup.library $(OUT)/PrismMon $(OUT)/PrismNetPrefs $(OUT)/bsdtest $(OUT)/bsdstress $(OUT)/ugtest $(OUT)/ugdump $(OUT)/rsquery $(OUT)/rscfg $(OUT)/pnpreload $(OUT)/pntest $(OUT)/carddiag

$(OBJ)/liblwip.a: $(LWIP_OBJ)
	$(AR) rcs $@ $^

$(OBJ)/lwip/%.o: lwip/src/%.c port/include/lwipopts.h port/include/arch/cc.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COREOPT) -c -o $@ $<

$(OBJ)/port/%.o: port/%.c port/include/lwipopts.h $(wildcard port/include/prismnet/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COREOPT) -c -o $@ $<

$(OBJ)/port/%.o: port/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c -o $@ $<

$(OBJ)/core/%.o: core/%.c core/core.h core/config.h port/include/lwipopts.h $(wildcard port/include/prismnet/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COREOPT) -c -o $@ $<

$(OBJ)/tools/%.o: tools/%.c port/include/lwipopts.h $(wildcard port/include/prismnet/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Network filesystems (docs/NFSCLIENT.md, docs/SMBCLIENT.md): a shared DOS
# handler core in tools/netfs plus one protocol backend per handler.
# prismnfs-handler = core + NFS backend. A DOS handler has no startup code:
# nfs_start.o must come first; libnix is linked only for its pure helpers.
NETFS_CFLAGS = $(CFLAGS) -Itools/netfs -Itools/nfs
NETFS_H = $(wildcard tools/netfs/*.h) $(wildcard tools/nfs/*.h)
NFS_OBJ = $(OBJ)/tools/nfs/nfs_start.o $(OBJ)/tools/netfs/core.o \
          $(OBJ)/tools/netfs/names.o $(OBJ)/tools/netfs/net_amiga.o $(OBJ)/tools/netfs/nbname.o \
          $(OBJ)/tools/nfs/backend.o $(OBJ)/tools/nfs/nfs3.o \
          $(OBJ)/tools/nfs/rpc.o $(OBJ)/tools/nfs/xdr.o
$(OBJ)/tools/netfs/%.o: tools/netfs/%.c $(NETFS_H)
	@mkdir -p $(dir $@)
	$(CC) $(NETFS_CFLAGS) -c -o $@ $<
$(OBJ)/tools/nfs/%.o: tools/nfs/%.c $(NETFS_H)
	@mkdir -p $(dir $@)
	$(CC) $(NETFS_CFLAGS) -c -o $@ $<
$(OBJ)/tools/nfs/%.o: tools/nfs/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c -o $@ $<
$(OUT)/prismnfs-handler: $(NFS_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -nostartfiles -s -o $@ $^
$(OUT)/ShareUnmount: tools/netfs/shareunmount.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -s -o $@ $<
$(OUT)/nfstest: tools/netfs/nfstest.c tools/smb/crypto.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Itools/netfs -Itools/smb $(LDFLAGS) -s -o $@ $^
$(OUT)/hog: tools/hog.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -s -o $@ $<
$(OUT)/walk: tools/netfs/walk.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -s -o $@ $<
$(OUT)/parenttest: tools/netfs/parenttest.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -s -o $@ $<
nfs: $(OUT)/prismnfs-handler $(OUT)/ShareUnmount $(OUT)/nfstest $(OUT)/parenttest $(OUT)/hog $(OUT)/walk

# prismsmb-handler = the same core (built again: handles hold a share path,
# FS_FHSIZE=260) + the SMB2 backend (docs/SMBCLIENT.md)
SMB_CFLAGS = $(CFLAGS) -Itools/netfs -Itools/smb -DFS_FHSIZE=260
SMB_H = $(wildcard tools/netfs/*.h) $(wildcard tools/smb/*.h)
SMB_OBJ = $(OBJ)/tools/smb/smb_start.o $(OBJ)/tools/smb/core.o \
          $(OBJ)/tools/netfs/names.o $(OBJ)/tools/netfs/net_amiga.o $(OBJ)/tools/netfs/nbname.o \
          $(OBJ)/tools/smb/backend.o $(OBJ)/tools/smb/smb2.o $(OBJ)/tools/smb/crypto.o           $(OBJ)/tools/smb/passreq.o
$(OBJ)/tools/smb/core.o: tools/netfs/core.c $(SMB_H)
	@mkdir -p $(dir $@)
	$(CC) $(SMB_CFLAGS) -c -o $@ $<
$(OBJ)/tools/smb/%.o: tools/smb/%.c $(SMB_H)
	@mkdir -p $(dir $@)
	$(CC) $(SMB_CFLAGS) -c -o $@ $<
$(OBJ)/tools/smb/%.o: tools/smb/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c -o $@ $<
$(OUT)/prismsmb-handler: $(SMB_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -nostartfiles -s -o $@ $^
$(OUT)/hashbench: tools/smb/hashbench.c tools/smb/crypto.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Itools/netfs -Itools/smb $(LDFLAGS) -s -o $@ $^    # the handler's flags
$(OUT)/SMBShares: tools/smb/smbshares.c tools/smb/smb2.c tools/smb/crypto.c tools/netfs/net_amiga.c tools/netfs/nbname.c $(SMB_H)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Itools/netfs -Itools/smb $(LDFLAGS) -s -o $@ tools/smb/smbshares.c tools/smb/smb2.c tools/smb/crypto.c tools/netfs/net_amiga.c tools/netfs/nbname.c
smb: $(OUT)/prismsmb-handler $(OUT)/hashbench $(OUT)/SMBShares

# Host tests of the SMB layer (native gcc, WSL): crypto vectors, smbtool
SMB_HOST = -O1 -Wall -Itools/netfs -Itools/smb
# Parser checks with crafted replies, under AddressSanitizer (no network)
smbunit: build/smbunit
build/smbunit: tools/smb/smbunit.c tools/smb/smb2.c tools/smb/crypto.c $(SMB_H)
	@mkdir -p build
	gcc -O1 -g -fsanitize=address -Wall -Wno-unused-function -Itools/netfs -Itools/smb -o $@ tools/smb/smbunit.c tools/smb/crypto.c tools/netfs/net_posix.c tools/netfs/nbname.c
xdrtest: build/xdrtest
build/xdrtest: tools/nfs/xdrtest.c tools/nfs/xdr.c tools/nfs/xdr.h
	@mkdir -p build
	gcc -O1 -g -fsanitize=address -Wall -Itools/netfs -Itools/nfs -o $@ tools/nfs/xdrtest.c tools/nfs/xdr.c
cryptotest: build/cryptotest
build/cryptotest: tools/smb/crypto.c tools/smb/cryptotest.c $(SMB_H)
	@mkdir -p build
	gcc $(SMB_HOST) -o $@ tools/smb/crypto.c tools/smb/cryptotest.c
smbtool: build/smbtool
build/smbtool: tools/smb/smb2.c tools/smb/crypto.c tools/smb/smbtool.c tools/netfs/net_posix.c tools/netfs/nbname.c $(SMB_H)
	@mkdir -p build
	gcc $(SMB_HOST) -o $@ tools/smb/smb2.c tools/smb/crypto.c tools/smb/smbtool.c tools/netfs/net_posix.c tools/netfs/nbname.c

# Host test of the NFS protocol layer (native gcc, run in WSL):
#   make nfstool && build/nfstool 192.168.1.10:/volume1/amigatest ls
NFS_HOST_SRC = tools/nfs/xdr.c tools/nfs/rpc.c tools/nfs/nfs3.c tools/netfs/names.c \
               tools/netfs/net_posix.c tools/netfs/nbname.c tools/nfs/nfstool.c
nfstool: build/nfstool
build/nfstool: $(NFS_HOST_SRC) $(NETFS_H)
	@mkdir -p build
	gcc -O1 -Wall -Itools/netfs -Itools/nfs -o $@ $(NFS_HOST_SRC)

# Strip at link time (-s): m68k-amigaos-strip corrupts this binary's relocs.
$(OUT)/pntest: $(OBJ)/tools/pntest.o $(PORT_OBJ) $(OBJ)/liblwip.a $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

# -u ___stkinit links libnix's swapstack.o, which gives the core the stack
# __stack asks for. Without it __stack is silently ignored and the core runs
# on the 4 KB a new process gets: deep enough paths (ping's ICMP) overran it
# and corrupted the memory below, found at exit as guru 8100 0005.
$(OUT)/PrismNet: $(CORE_OBJ) $(PORT_OBJ) $(OBJ)/liblwip.a $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -Wl,-u,___stkinit -s -o $@ $^

$(OUT)/carddiag: $(OBJ)/tools/carddiag.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

# bsdsocket.library: no startup code (lib_start.o must come first); libnix is
# linked only for its pure helpers (memcpy, strlen, ...).
$(OBJ)/lib/%.o: lib/%.c port/include/prismnet/pnmsg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/lib/%.o: lib/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c -o $@ $<

$(OUT)/bsdsocket.library: $(OBJ)/lib/lib_start.o $(OBJ)/lib/bsdsocket.o $(OBJ)/lib/support.o
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -nostartfiles -s -o $@ $^

# bsdtest is an ordinary bsdsocket client built against the NDK headers only.
$(OBJ)/test/%.o: tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -Os -noixemul -Wall -Wno-pointer-sign -c -o $@ $<

$(OUT)/bsdtest: $(OBJ)/test/bsdtest.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

$(OUT)/bsdstress: $(OBJ)/test/bsdstress.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

$(OUT)/ugtest: $(OBJ)/test/ugtest.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

$(OUT)/ugdump: $(OBJ)/test/ugdump.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

$(OUT)/rsquery: $(OBJ)/test/rsquery.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

$(OUT)/rscfg: $(OBJ)/test/rscfg.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

$(OUT)/pnpreload: $(OBJ)/test/pnpreload.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

$(OUT)/usergroup.library: $(OBJ)/lib/ug_start.o $(OBJ)/lib/usergroup.o $(OBJ)/lib/support.o
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -nostartfiles -s -o $@ $^

# Small-memory core for 68000 machines with about 1 MB: out/small/PrismNet
# (PN_SMALL in lwipopts.h: smaller window, buffers, heap, tables and stack).
small:
	$(MAKE) OBJ=build/objsmall OUT=out/small CFLAGS="$(CFLAGS) -DPN_SMALL" 		COREOPT="$(SMALLOPT) -DPN_CORE" out/small/PrismNet
SMALLOPT ?= -Os

# 68020+ build of the core: out/020/PrismNet. Not shipped: on the emulated
# 68030 it measured no faster than the 68000 build (the core is no longer the
# bottleneck), see docs/PERFORMANCE.md.
core020:
	$(MAKE) CPU=-m68020 OBJ=build/obj020 OUT=out/020 out/020/PrismNet

# Test build with the passive loop probe: out/diag/PrismNet + out/diag/pndiag.
diag:
	$(MAKE) OBJ=build/objdiag OUT=out/diag CFLAGS="$(CFLAGS) -DPN_DIAGPORT" out/diag/PrismNet
	$(CC) $(CPU) -Os -noixemul -Wall -s -o out/diag/pndiag tools/pndiag.c
	$(CC) $(CPU) -Os -noixemul -Wall -Wno-pointer-sign -Iport/include -s -o out/diag/s2stat tools/s2stat.c

clean:
	rm -rf build $(OUT)

.PHONY: small servers all clean core020 diag nettools pktlog nfs nfstool smb cryptotest smbtool

# Diagnostic core that logs every frame (PrismNet LOG shows them).
pktlog:
	$(MAKE) OBJ=build/objpkt OUT=out/pkt CFLAGS="$(CFLAGS) -DPN_PKTLOG" out/pkt/PrismNet

# GUIs (GadTools; talk to the core directly through port/pnclient.c)
$(OBJ)/gui/%.o: tools/%.c $(wildcard port/include/prismnet/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -Os -noixemul -Wall -Wno-pointer-sign -Iport/include -c -o $@ $<

$(OBJ)/gui/prismprefs.o: tools/prefsshares_io.c tools/prefsshares_gui.c tools/prefsservices_io.c tools/prefsservices_gui.c
$(OUT)/PrismMon: $(OBJ)/gui/prismmon.o $(OBJ)/gui/tabs.o $(OBJ)/port/pnclient.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

$(OUT)/PrismNetPrefs: $(OBJ)/gui/prismprefs.o $(OBJ)/gui/tabs.o $(STDIO_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

# Servers: inetd and the services it starts (plain bsdsocket programs, any stack)
SERVERS = $(OUT)/inetd $(OUT)/ftpd $(OUT)/telnetd
servers: $(SERVERS)
$(OBJ)/srv/%.o: tools/srv/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -Os -noixemul -Wall -Wno-pointer-sign -c -o $@ $<
$(OUT)/inetd: $(OBJ)/srv/inetd.o $(STDIO_OBJ)
$(OUT)/ftpd: $(OBJ)/srv/ftpd.o $(STDIO_OBJ)
$(OUT)/telnetd: $(OBJ)/srv/telnetd.o $(STDIO_OBJ)
$(SERVERS):
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^

# Network tools (plain bsdsocket clients, any stack)
NETTOOLS = $(OUT)/ping $(OUT)/traceroute $(OUT)/nslookup $(OUT)/httpbench
nettools: $(NETTOOLS)
$(OBJ)/net/%.o: tools/net/%.c tools/net/nettools.h $(wildcard port/include/prismnet/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -Os -noixemul -Wall -Wno-pointer-sign -Iport/include -c -o $@ $<
$(OUT)/ping: $(OBJ)/net/ping.o $(STDIO_OBJ)
$(OUT)/traceroute: $(OBJ)/net/traceroute.o $(STDIO_OBJ)
$(OUT)/httpbench: $(OBJ)/net/httpbench.o $(STDIO_OBJ)
$(OUT)/nslookup: $(OBJ)/net/nslookup.o $(OBJ)/port/pnclient.o $(STDIO_OBJ)
$(NETTOOLS):
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -s -o $@ $^
